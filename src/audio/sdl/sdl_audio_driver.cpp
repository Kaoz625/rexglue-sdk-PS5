/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <array>
#include <cstring>

#include <rex/assert.h>
#include <rex/audio/conversion.h>
#include <rex/audio/downmix.h>
#include <rex/audio/flags.h>
#include <rex/audio/sdl/sdl_audio_driver.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/logging.h>
#include <rex/perf/counter.h>
#include <SDL3/SDL.h>

#if REX_PLATFORM_PS5
#include <atomic>
#include <chrono>
#include <memory>
#include <thread>
extern "C" int sceAudioOutInit(void);
extern "C" int sceAudioOutOpen(int32_t user, int32_t type, int32_t index, uint32_t len, uint32_t freq,
                               uint32_t param);
extern "C" int sceAudioOutClose(int32_t port);

namespace {
// 15 Nov 2026: MK vs DC on PHAT5 (FW 10.20, HDMI just switched to 1080p 119.88 Hz) never came back from opening the
// audio port, so the guest thread that registers its audio client hung with it and the console wedged when the title
// was killed. Probe the same open SDL's PS5 driver does on a helper thread first: if it does not return within
// audio_open_timeout_ms, the driver fails (the game runs silent, AudioSystem drops frames) instead of hanging. A
// probe that never returns stays parked on its own thread; no SDL lock is held by it.
REXCVAR_DEFINE_INT32(audio_open_timeout_ms, 5000, "Audio",
                     "PS5: give up on the audio port after this many ms and run silent (0 = wait forever)");

bool Ps5AudioPortAnswers() {
  const int timeout_ms = REXCVAR_GET(audio_open_timeout_ms);
  if (timeout_ms <= 0) return true;
  auto state = std::make_shared<std::atomic<int>>(0);  // 0 waiting, 1 opened, 2 failed
  std::thread([state] {
    const int init = sceAudioOutInit();
    if (init < 0 && init != int(0x8026000e)) {
      state->store(2);
      return;
    }
    const int port = sceAudioOutOpen(0xff, 0, 0, 256, 48000, 1);
    if (port >= 0) sceAudioOutClose(port);
    state->store(port >= 0 ? 1 : 2);
  }).detach();
  const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
  while (state->load() == 0 && std::chrono::steady_clock::now() < end)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  const int result = state->load();
  if (result == 0) {
    REXAPU_ERROR("PS5 audio: sceAudioOutOpen did not return within {} ms - running without sound", timeout_ms);
    return false;
  }
  if (result == 2) REXAPU_WARN("PS5 audio: the probe open failed; SDL will try anyway");
  return true;
}
}  // namespace
#endif

REXCVAR_DEFINE_BOOL(audio_mute, false, "Audio", "Mute audio output");

namespace rex::audio::sdl {

SDLAudioDriver::SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore)
    : AudioDriver(memory), semaphore_(semaphore) {}

SDLAudioDriver::~SDLAudioDriver() {
  assert_true(frames_queued_.empty());
  assert_true(frames_unused_.empty());
}

bool SDLAudioDriver::Initialize() {
  if (InitializeDevice()) return true;
  // Release only this driver's partial SDL resources. Do not switch the global
  // backend: other clients may still have working devices.
  Shutdown();
  silent_sink_ = std::make_unique<ClockedAudioSink>([this] {
    if (!semaphore_->Release(1, nullptr)) {
      REXAPU_WARN("Silent audio: unable to return consumed frame permit");
    }
  });
  if (!silent_sink_->Start()) {
    silent_sink_.reset();
    REXAPU_ERROR("Unable to start silent audio fallback worker");
    return false;
  }
  REXAPU_WARN("Audio device unavailable; using clocked silent output for this client");
  return true;
}

bool SDLAudioDriver::InitializeDevice() {
#if REX_PLATFORM_PS5
  if (!Ps5AudioPortAnswers()) return false;
#endif
  // Set audio category for proper OS audio handling
  SDL_SetHint(SDL_HINT_AUDIO_CATEGORY, "playback");

  // Set app name for audio device identification
  SDL_SetAppMetadataProperty(SDL_PROP_APP_METADATA_NAME_STRING, "rexglue");

  if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
    REXAPU_ERROR("SDL_InitSubSystem(SDL_INIT_AUDIO) failed: {}", SDL_GetError());
    return false;
  }
  sdl_initialized_ = true;

  SDL_AudioSpec desired_spec = {};
  SDL_AudioSpec obtained_spec = {};
  desired_spec.freq = frame_frequency_;
  desired_spec.format = SDL_AUDIO_F32LE;
  desired_spec.channels = frame_channels_;
  sdl_device_channels_ = frame_channels_;
  sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                          SDLCallback, this);
  if (!sdl_stream_) {
    REXAPU_ERROR("SDL_OpenAudioDeviceStream() failed: {}", SDL_GetError());
    return false;
  }

  SDL_AudioDeviceID sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
  if (!sdl_device) {
    REXAPU_ERROR("SDL_GetAudioStreamDevice() failed: {}", SDL_GetError());
    return false;
  }

  if (!SDL_GetAudioDeviceFormat(sdl_device, &obtained_spec, NULL)) {
    REXAPU_WARN("SDL_GetAudioDeviceFormat() failed: {}", SDL_GetError());
    obtained_spec = desired_spec;
  }

  // A 1-channel device gets the stereo fold too, then SDL collapses to mono.
  // Handing it a 6ch stream instead would use SDL's own downmix.
  if (obtained_spec.channels <= 2) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
    desired_spec.channels = 2;
    sdl_device_channels_ = 2;
    sdl_stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &desired_spec,
                                            SDLCallback, this);
    if (!sdl_stream_) {
      REXAPU_ERROR("SDL_OpenAudioDeviceStream() stereo fallback failed: {}", SDL_GetError());
      return false;
    }
    sdl_device = SDL_GetAudioStreamDevice(sdl_stream_);
    if (!sdl_device) {
      REXAPU_ERROR("SDL_GetAudioStreamDevice() failed after stereo fallback: {}", SDL_GetError());
      return false;
    }
  }

  // The endpoint layout decides which mix the callback runs, and it is the
  // first thing worth knowing when a report says the balance is wrong on one
  // speaker setup and right on another.
  const char* device_name = SDL_GetAudioDeviceName(sdl_device);
  REXAPU_INFO("audio endpoint '{}': {} ch, {} Hz, format 0x{:04X}; submitting {} ch",
              device_name ? device_name : "?", obtained_spec.channels, obtained_spec.freq,
              static_cast<uint32_t>(obtained_spec.format), static_cast<int>(sdl_device_channels_));

  if (!SDL_ResumeAudioDevice(sdl_device)) {
    REXAPU_ERROR("SDL_ResumeAudioDevice() failed: {}", SDL_GetError());
    return false;
  }

  return true;
}

void SDLAudioDriver::SubmitFrame(uint32_t frame_ptr) {
  if (silent_sink_) {
    if (!silent_sink_->Submit()) {
      REXAPU_WARN("Silent audio: frame queue is stopped or full");
    }
    return;
  }
  const auto input_frame = memory_->TranslateVirtual<float*>(frame_ptr);
  float* output_frame;
  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    if (frames_unused_.empty()) {
      output_frame = new float[frame_samples_];
    } else {
      output_frame = frames_unused_.top();
      frames_unused_.pop();
    }
  }

  std::memcpy(output_frame, input_frame, frame_samples_ * sizeof(float));

  static uint32_t sdl_submit_count = 0;
  if (sdl_submit_count < 10) {
    REXAPU_DEBUG("SDLAudioDriver::SubmitFrame: frame_ptr={:08X} queued_count={}", frame_ptr,
                 frames_queued_.size() + 1);
    sdl_submit_count++;
  }

  {
    std::unique_lock<std::mutex> guard(frames_mutex_);
    frames_queued_.push(output_frame);
    PROFILE_BUFFER_QUEUE_DEPTH(static_cast<int64_t>(frames_queued_.size()));
  }
}

void SDLAudioDriver::Shutdown() {
  // Join before the owning AudioSystem destroys this client's semaphore.
  if (silent_sink_) {
    silent_sink_->Stop();
    silent_sink_.reset();
  }
  if (sdl_stream_) {
    SDL_DestroyAudioStream(sdl_stream_);
    sdl_stream_ = nullptr;
  }
  if (sdl_initialized_) {
    SDL_QuitSubSystem(SDL_INIT_AUDIO);
    sdl_initialized_ = false;
  }
  std::unique_lock<std::mutex> guard(frames_mutex_);
  while (!frames_unused_.empty()) {
    delete[] frames_unused_.top();
    frames_unused_.pop();
  }
  while (!frames_queued_.empty()) {
    delete[] frames_queued_.front();
    frames_queued_.pop();
  }
}

void SDLAudioDriver::SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                                 [[maybe_unused]] int total_amount) {
  SCOPE_profile_cpu_f("apu");
  if (!userdata || !stream) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback called with nullptr.");
    return;
  }
  const auto driver = static_cast<SDLAudioDriver*>(userdata);
  const int sample_count =
      static_cast<int>(channel_samples_ * std::max<uint8_t>(driver->sdl_device_channels_, 1));
  const int len = static_cast<int>(sizeof(float) * sample_count);
  float* data = SDL_stack_alloc(float, sample_count);
  if (!data) {
    REXAPU_ERROR("SDLAudioDriver::SDLCallback failed to allocate {} samples", sample_count);
    return;
  }
  // Snapshot once. A change mid-callback would split the frame across two mixes.
  const StereoFold fold = GetStereoFold();
  const SurroundMix mix = GetSurroundMix();
  const float gain = GetOutputGain();
  while (additional_amount > 0) {
    static uint32_t sdl_callback_count = 0;
    std::unique_lock<std::mutex> guard(driver->frames_mutex_);
    if (driver->frames_queued_.empty()) {
      if (sdl_callback_count < 10) {
        REXAPU_DEBUG("SDLCallback: no frames queued (silence)");
        sdl_callback_count++;
      }
      std::memset(data, 0, len);
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed while filling silence: {}", SDL_GetError());
        break;
      }
      additional_amount -= len;
    } else {
      auto buffer = driver->frames_queued_.front();
      driver->frames_queued_.pop();
      if (REXCVAR_GET(audio_mute)) {
        std::memset(data, 0, len);
      } else {
        switch (driver->sdl_device_channels_) {
          case 2:
            conversion::sequential_6_BE_to_interleaved_2_LE(data, buffer, channel_samples_, fold,
                                                            gain);
            break;
          case 6:
            conversion::sequential_6_BE_to_interleaved_6_LE(data, buffer, channel_samples_, mix,
                                                            gain);
            break;
          default:
            assert_unhandled_case(driver->sdl_device_channels_);
            break;
        }
      }
      if (!SDL_PutAudioStreamData(stream, data, len)) {
        REXAPU_ERROR("SDL_PutAudioStreamData() failed: {}", SDL_GetError());
        driver->frames_unused_.push(buffer);
        break;
      }
      driver->frames_unused_.push(buffer);

      auto ret = driver->semaphore_->Release(1, nullptr);
      assert_true(ret);
      additional_amount -= len;
    }
  }
  SDL_stack_free(data);
}

}  // namespace rex::audio::sdl
