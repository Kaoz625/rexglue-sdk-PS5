/**
 * XAudio2 audio backend: for UWP hosts (Xbox One Dev Mode), where SDL3 audio cannot start ("Unable to create
 * Helper Window Class"), and usable on desktop Windows too. Same contract as the SDL driver: every frame the
 * guest submits is played, and the frame semaphore is released when its buffer finishes - that release is
 * the clock the guest's audio thread waits on (with the NOP backend Army of Two's movie code read a null
 * pointer, x360-native 29 Oct 2026). Modelled on Xenia's XAudio2 backend (BSD, Ben Vanik and contributors).
 */
#pragma once

#include <rex/audio/audio_system.h>

namespace rex::audio::xaudio2 {

class XAudio2AudioSystem : public AudioSystem {
 public:
  explicit XAudio2AudioSystem(runtime::FunctionDispatcher* function_dispatcher);
  ~XAudio2AudioSystem() override;

  static bool IsAvailable() { return true; }

  static std::unique_ptr<AudioSystem> Create(runtime::FunctionDispatcher* function_dispatcher);

  X_STATUS CreateDriver(size_t index, rex::thread::Semaphore* semaphore,
                        AudioDriver** out_driver) override;
  void DestroyDriver(AudioDriver* driver) override;
};

}  // namespace rex::audio::xaudio2
