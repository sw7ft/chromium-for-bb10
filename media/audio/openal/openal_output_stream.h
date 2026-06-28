// Copyright 2026 SW7FT. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_OPENAL_OPENAL_OUTPUT_STREAM_H_
#define MEDIA_AUDIO_OPENAL_OPENAL_OUTPUT_STREAM_H_

#include <stdint.h>

#include <AL/al.h>
#include <AL/alc.h>

#include <memory>
#include <vector>

#include "media/audio/audio_io.h"
#include "media/base/audio_parameters.h"
#include "media/base/fake_audio_worker.h"

namespace media {

class AudioBus;
class AudioManagerBase;

// AudioOutputStream backed by OpenAL. OpenAL is the only working audio output
// path on QNX/BB10 (the device's native sound stack, also used by BerryCore's
// play-audio). Audio is pulled from the source callback on the audio worker
// thread, converted to interleaved int16, and streamed to the hardware using
// OpenAL's buffer-queueing model (queue N buffers, recycle them as the source
// reports them played out).
class OpenALOutputStream : public AudioOutputStream {
 public:
  OpenALOutputStream(AudioManagerBase* manager, const AudioParameters& params);

  OpenALOutputStream(const OpenALOutputStream&) = delete;
  OpenALOutputStream& operator=(const OpenALOutputStream&) = delete;

  ~OpenALOutputStream() override;

  // AudioOutputStream:
  bool Open() override;
  void Start(AudioSourceCallback* callback) override;
  void Stop() override;
  void SetVolume(double volume) override;
  void GetVolume(double* volume) override;
  void Close() override;
  void Flush() override;

 private:
  // Runs on the worker task runner; services the OpenAL buffer queue.
  void Pump(base::TimeTicks ideal_time, base::TimeTicks now);

  // Pulls one buffer worth of audio from |callback_| into |al_buffer| and
  // queues it on the source. |queued_buffers| is the number of buffers already
  // queued ahead of this one (used to estimate output delay).
  void FillAndQueue(ALuint al_buffer, int queued_buffers);

  AudioManagerBase* const manager_;
  const AudioParameters params_;
  const ALenum al_format_;

  AudioSourceCallback* callback_ = nullptr;  // Not owned.
  double volume_ = 1.0;

  ALuint source_ = 0;
  std::vector<ALuint> buffers_;
  std::unique_ptr<AudioBus> audio_bus_;
  std::vector<int16_t> interleaved_;
  bool primed_ = false;

  FakeAudioWorker worker_;
};

}  // namespace media

#endif  // MEDIA_AUDIO_OPENAL_OPENAL_OUTPUT_STREAM_H_
