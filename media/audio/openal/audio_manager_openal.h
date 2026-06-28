// Copyright 2026 SW7FT. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_OPENAL_AUDIO_MANAGER_OPENAL_H_
#define MEDIA_AUDIO_OPENAL_AUDIO_MANAGER_OPENAL_H_

#include <AL/alc.h>

#include <memory>
#include <string>

#include "media/audio/audio_manager_base.h"

namespace media {

// AudioManager for QNX/BB10 backed by OpenAL. Owns the single ALC device and
// context for the process; OpenALOutputStream instances render through it.
// Microphone input is served by OpenALInputStream via the ALC capture
// extension; if capture is unavailable, input requests fall back to fake
// streams.
class MEDIA_EXPORT AudioManagerOpenAL : public AudioManagerBase {
 public:
  AudioManagerOpenAL(std::unique_ptr<AudioThread> audio_thread,
                     AudioLogFactory* audio_log_factory);

  AudioManagerOpenAL(const AudioManagerOpenAL&) = delete;
  AudioManagerOpenAL& operator=(const AudioManagerOpenAL&) = delete;

  ~AudioManagerOpenAL() override;

  // AudioManager:
  bool HasAudioOutputDevices() override;
  bool HasAudioInputDevices() override;
  void GetAudioInputDeviceNames(AudioDeviceNames* device_names) override;
  const char* GetName() override;

  // AudioManagerBase:
  AudioOutputStream* MakeLinearOutputStream(
      const AudioParameters& params,
      const LogCallback& log_callback) override;
  AudioOutputStream* MakeLowLatencyOutputStream(
      const AudioParameters& params,
      const std::string& device_id,
      const LogCallback& log_callback) override;
  AudioInputStream* MakeLinearInputStream(
      const AudioParameters& params,
      const std::string& device_id,
      const LogCallback& log_callback) override;
  AudioInputStream* MakeLowLatencyInputStream(
      const AudioParameters& params,
      const std::string& device_id,
      const LogCallback& log_callback) override;
  AudioParameters GetInputStreamParameters(
      const std::string& device_id) override;

 protected:
  AudioParameters GetPreferredOutputStreamParameters(
      const std::string& output_device_id,
      const AudioParameters& input_params) override;

 private:
  ALCdevice* device_ = nullptr;
  ALCcontext* context_ = nullptr;
};

}  // namespace media

#endif  // MEDIA_AUDIO_OPENAL_AUDIO_MANAGER_OPENAL_H_
