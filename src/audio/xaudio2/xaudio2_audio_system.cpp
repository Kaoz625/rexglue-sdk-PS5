/**
 * XAudio2 audio backend. See include/rex/audio/xaudio2/xaudio2_audio_system.h.
 */

#include <rex/audio/xaudio2/xaudio2_audio_system.h>

#include <array>
#include <atomic>
#include <cstring>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <mmreg.h>
#include <xaudio2.h>

#include <rex/assert.h>
#include <rex/audio/audio_driver.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/logging.h>
#include <rex/thread.h>

namespace rex::audio::xaudio2 {

namespace {

// The guest's frame: 6 channels x 256 samples, sequential, big-endian float, 48 kHz.
constexpr uint32_t kFrequency = 48000;
constexpr uint32_t kChannels = 6;
constexpr uint32_t kChannelSamples = 256;
constexpr uint32_t kFrameSamples = kChannels * kChannelSamples;
// Buffers in flight are bounded by the guest's semaphore (8 queued frames); this ring only has to be
// larger than that so a buffer is never rewritten while XAudio2 still reads it.
constexpr size_t kRingSize = 32;

class XAudio2AudioDriver final : public AudioDriver, public IXAudio2VoiceCallback {
 public:
  XAudio2AudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
      : AudioDriver(memory), semaphore_(semaphore) {}
  ~XAudio2AudioDriver() override { Shutdown(); }

  bool Initialize() {
    // The audio driver is created on a guest-runtime thread where COM is not initialized: without this
    // CreateMasteringVoice fails with CO_E_NOTINITIALIZED (800401F0, 29 Oct). Already-initialized is fine.
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    HRESULT hr = XAudio2Create(&xaudio2_, 0, XAUDIO2_DEFAULT_PROCESSOR);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2Create failed: {:08X}", static_cast<uint32_t>(hr));
      return false;
    }
    hr = xaudio2_->CreateMasteringVoice(&mastering_voice_);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2 CreateMasteringVoice failed: {:08X}", static_cast<uint32_t>(hr));
      return false;
    }
    XAUDIO2_VOICE_DETAILS details{};
    mastering_voice_->GetVoiceDetails(&details);

    WAVEFORMATEXTENSIBLE format{};
    format.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    format.Format.nChannels = kChannels;
    format.Format.nSamplesPerSec = kFrequency;
    format.Format.wBitsPerSample = 32;
    format.Format.nBlockAlign = kChannels * sizeof(float);
    format.Format.nAvgBytesPerSec = kFrequency * format.Format.nBlockAlign;
    format.Format.cbSize = sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX);
    format.Samples.wValidBitsPerSample = 32;
    format.dwChannelMask = SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT | SPEAKER_FRONT_CENTER |
                           SPEAKER_LOW_FREQUENCY | SPEAKER_BACK_LEFT | SPEAKER_BACK_RIGHT;
    format.SubFormat = KSDATAFORMAT_SUBTYPE_IEEE_FLOAT;
    // XAudio2 maps 5.1 onto whatever the mastering voice has (stereo TV speakers included).
    hr = xaudio2_->CreateSourceVoice(&source_voice_, &format.Format, 0, XAUDIO2_DEFAULT_FREQ_RATIO, this);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2 CreateSourceVoice failed: {:08X}", static_cast<uint32_t>(hr));
      return false;
    }
    hr = source_voice_->Start(0);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2 Start failed: {:08X}", static_cast<uint32_t>(hr));
      return false;
    }
    REXAPU_INFO("XAudio2: output {} ch at {} Hz; submitting 5.1 float at {} Hz",
                details.InputChannels, details.InputSampleRate, kFrequency);
    return true;
  }

  void SubmitFrame(uint32_t frame_ptr) override {
    const auto* input = memory_->TranslateVirtual<const float*>(frame_ptr);
    float* output = ring_[next_].data();
    next_ = (next_ + 1) % kRingSize;
    conversion::sequential_6_BE_to_interleaved_6_LE(output, input, kChannelSamples, GetSurroundMix(),
                                                    GetOutputGain());
    XAUDIO2_BUFFER buffer{};
    buffer.AudioBytes = kFrameSamples * sizeof(float);
    buffer.pAudioData = reinterpret_cast<const BYTE*>(output);
    HRESULT hr = source_voice_->SubmitSourceBuffer(&buffer);
    if (FAILED(hr)) {
      REXAPU_ERROR("XAudio2 SubmitSourceBuffer failed: {:08X}", static_cast<uint32_t>(hr));
      // Keep the guest's clock running even if this frame is lost.
      semaphore_->Release(1, nullptr);
    }
  }

  void Shutdown() {
    if (source_voice_) {
      source_voice_->Stop(0);
      source_voice_->DestroyVoice();
      source_voice_ = nullptr;
    }
    if (mastering_voice_) {
      mastering_voice_->DestroyVoice();
      mastering_voice_ = nullptr;
    }
    if (xaudio2_) {
      xaudio2_->Release();
      xaudio2_ = nullptr;
    }
  }

  // IXAudio2VoiceCallback: a buffer finished - the guest may submit the next frame.
  void STDMETHODCALLTYPE OnBufferEnd(void*) override { semaphore_->Release(1, nullptr); }
  void STDMETHODCALLTYPE OnVoiceProcessingPassStart(UINT32) override {}
  void STDMETHODCALLTYPE OnVoiceProcessingPassEnd() override {}
  void STDMETHODCALLTYPE OnStreamEnd() override {}
  void STDMETHODCALLTYPE OnBufferStart(void*) override {}
  void STDMETHODCALLTYPE OnLoopEnd(void*) override {}
  void STDMETHODCALLTYPE OnVoiceError(void*, HRESULT error) override {
    REXAPU_ERROR("XAudio2 voice error {:08X}", static_cast<uint32_t>(error));
  }

 private:
  rex::thread::Semaphore* semaphore_;
  IXAudio2* xaudio2_ = nullptr;
  IXAudio2MasteringVoice* mastering_voice_ = nullptr;
  IXAudio2SourceVoice* source_voice_ = nullptr;
  std::array<std::array<float, kFrameSamples>, kRingSize> ring_{};
  size_t next_ = 0;
};

}  // namespace

std::unique_ptr<AudioSystem> XAudio2AudioSystem::Create(runtime::FunctionDispatcher* function_dispatcher) {
  return std::make_unique<XAudio2AudioSystem>(function_dispatcher);
}

XAudio2AudioSystem::XAudio2AudioSystem(runtime::FunctionDispatcher* function_dispatcher)
    : AudioSystem(function_dispatcher) {}

XAudio2AudioSystem::~XAudio2AudioSystem() = default;

X_STATUS XAudio2AudioSystem::CreateDriver([[maybe_unused]] size_t index, rex::thread::Semaphore* semaphore,
                                          AudioDriver** out_driver) {
  assert_not_null(out_driver);
  auto driver = new XAudio2AudioDriver(memory_, semaphore);
  if (!driver->Initialize()) {
    delete driver;
    return X_STATUS_UNSUCCESSFUL;
  }
  *out_driver = driver;
  return X_STATUS_SUCCESS;
}

void XAudio2AudioSystem::DestroyDriver(AudioDriver* driver) {
  delete driver;
}

}  // namespace rex::audio::xaudio2
