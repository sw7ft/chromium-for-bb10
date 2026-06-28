// Copyright 2026 SW7FT. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/openal/audio_manager_openal.h"

#include <AL/al.h>

#include <algorithm>
#include <memory>

#include "base/command_line.h"
#include "base/logging.h"
#include "media/audio/audio_device_name.h"
#include "media/audio/audio_io.h"
#include "media/audio/fake_audio_manager.h"
#include "media/audio/openal/openal_output_stream.h"
#include "media/audio/openal/qsa_input_stream.h"
#include "media/base/audio_parameters.h"
#include "media/base/channel_layout.h"
#include "media/base/media_switches.h"

namespace media {

namespace {

constexpr int kDefaultSampleRate = 48000;
constexpr int kDefaultOutputBufferSize = 2048;
constexpr int kDefaultInputBufferSize = 1024;

}  // namespace

AudioManagerOpenAL::AudioManagerOpenAL(std::unique_ptr<AudioThread> audio_thread,
                                       AudioLogFactory* audio_log_factory)
    : AudioManagerBase(std::move(audio_thread), audio_log_factory) {
  device_ = alcOpenDevice(nullptr);
  if (!device_) {
    LOG(ERROR) << "AudioManagerOpenAL: alcOpenDevice failed; audio disabled";
    return;
  }
  context_ = alcCreateContext(device_, nullptr);
  if (!context_ || alcMakeContextCurrent(context_) == ALC_FALSE) {
    LOG(ERROR) << "AudioManagerOpenAL: failed to create/activate ALC context";
    if (context_) {
      alcDestroyContext(context_);
      context_ = nullptr;
    }
    alcCloseDevice(device_);
    device_ = nullptr;
    return;
  }
  // The current context is process-global in OpenAL, so the worker thread that
  // drives buffer queueing shares this context without further setup.
  fprintf(stderr, "BerryShell: OpenAL audio output initialized\n");
}

AudioManagerOpenAL::~AudioManagerOpenAL() {
  alcMakeContextCurrent(nullptr);
  if (context_) {
    alcDestroyContext(context_);
  }
  if (device_) {
    alcCloseDevice(device_);
  }
}

bool AudioManagerOpenAL::HasAudioOutputDevices() {
  return device_ != nullptr;
}

bool AudioManagerOpenAL::HasAudioInputDevices() {
  // Microphone capture is provided by QSA (libasound), independent of the
  // OpenAL playback device. The Passport always has a mic; if the actual
  // capture open is denied (no record_audio permission) the input stream's
  // Open() fails gracefully, so advertising the device here is safe.
  return true;
}

void AudioManagerOpenAL::GetAudioInputDeviceNames(
    AudioDeviceNames* device_names) {
  DCHECK(device_names->empty());
  // Advertise a single default capture device when the mic path is available,
  // so getUserMedia({audio:true}) can find a device to open.
  if (HasAudioInputDevices()) {
    device_names->push_back(AudioDeviceName::CreateDefault());
  }
  fprintf(stderr, "BerryShell: GetAudioInputDeviceNames count=%d\n",
          static_cast<int>(device_names->size()));
}

const char* AudioManagerOpenAL::GetName() {
  return "OpenAL";
}

AudioOutputStream* AudioManagerOpenAL::MakeLinearOutputStream(
    const AudioParameters& params,
    const LogCallback& log_callback) {
  if (!device_) {
    return nullptr;
  }
  return new OpenALOutputStream(this, params);
}

AudioOutputStream* AudioManagerOpenAL::MakeLowLatencyOutputStream(
    const AudioParameters& params,
    const std::string& device_id,
    const LogCallback& log_callback) {
  if (!device_) {
    return nullptr;
  }
  return new OpenALOutputStream(this, params);
}

AudioInputStream* AudioManagerOpenAL::MakeLinearInputStream(
    const AudioParameters& params,
    const std::string& device_id,
    const LogCallback& log_callback) {
  return new QsaInputStream(this, params);
}

AudioInputStream* AudioManagerOpenAL::MakeLowLatencyInputStream(
    const AudioParameters& params,
    const std::string& device_id,
    const LogCallback& log_callback) {
  return new QsaInputStream(this, params);
}

AudioParameters AudioManagerOpenAL::GetInputStreamParameters(
    const std::string& device_id) {
  // Microphones are mono; OpenAL capture honours the requested format directly,
  // so advertise a mono low-latency stream.
  return AudioParameters(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                         ChannelLayoutConfig::Mono(), kDefaultSampleRate,
                         kDefaultInputBufferSize);
}

AudioParameters AudioManagerOpenAL::GetPreferredOutputStreamParameters(
    const std::string& output_device_id,
    const AudioParameters& input_params) {
  // Force stereo output: OpenAL's core formats only cover mono/stereo, and
  // Chromium's mixer happily downmixes anything to it.
  int sample_rate = kDefaultSampleRate;
  int buffer_size = kDefaultOutputBufferSize;
  if (input_params.IsValid()) {
    sample_rate = input_params.sample_rate();
    buffer_size = std::min(input_params.frames_per_buffer(), buffer_size);
  }
  return AudioParameters(AudioParameters::AUDIO_PCM_LOW_LATENCY,
                         ChannelLayoutConfig::Stereo(), sample_rate,
                         buffer_size);
}

// Platform factory consumed by AudioManager::Create(). Compiled only for QNX
// (see media/audio/BUILD.gn), replacing the Linux/fake factory.
std::unique_ptr<AudioManager> CreateAudioManager(
    std::unique_ptr<AudioThread> audio_thread,
    AudioLogFactory* audio_log_factory) {
  if (base::CommandLine::ForCurrentProcess()->HasSwitch(
          switches::kDisableAudioOutput)) {
    fprintf(stderr, "BerryShell: CreateAudioManager -> Fake (audio disabled)\n");
    return std::make_unique<FakeAudioManager>(std::move(audio_thread),
                                              audio_log_factory);
  }
  fprintf(stderr, "BerryShell: CreateAudioManager -> OpenAL\n");
  return std::make_unique<AudioManagerOpenAL>(std::move(audio_thread),
                                              audio_log_factory);
}

}  // namespace media
