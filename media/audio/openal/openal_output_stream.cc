// Copyright 2026 SW7FT. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/openal/openal_output_stream.h"

#include <algorithm>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/time/time.h"
#include "media/audio/audio_manager_base.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_sample_types.h"

namespace media {

namespace {

// Number of OpenAL buffers kept in flight. Four ~10-20ms buffers gives enough
// cushion to ride out scheduling jitter on the single-core-bound Krait without
// adding much latency.
constexpr int kNumBuffers = 4;

ALenum FormatForChannels(int channels) {
  return channels <= 1 ? AL_FORMAT_MONO16 : AL_FORMAT_STEREO16;
}

}  // namespace

OpenALOutputStream::OpenALOutputStream(AudioManagerBase* manager,
                                       const AudioParameters& params)
    : manager_(manager),
      params_(params),
      al_format_(FormatForChannels(params.channels())),
      audio_bus_(AudioBus::Create(params)),
      worker_(manager->GetWorkerTaskRunner(), params) {}

OpenALOutputStream::~OpenALOutputStream() = default;

bool OpenALOutputStream::Open() {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  if (!params_.IsValid()) {
    return false;
  }

  // Clear any stale error state, then allocate the source + buffer ring.
  alGetError();
  buffers_.assign(kNumBuffers, 0);
  alGenBuffers(kNumBuffers, buffers_.data());
  alGenSources(1, &source_);
  if (alGetError() != AL_NO_ERROR || source_ == 0) {
    LOG(ERROR) << "OpenALOutputStream: failed to allocate OpenAL source/buffers";
    if (source_) {
      alDeleteSources(1, &source_);
      source_ = 0;
    }
    alDeleteBuffers(kNumBuffers, buffers_.data());
    buffers_.clear();
    return false;
  }

  interleaved_.assign(
      static_cast<size_t>(params_.frames_per_buffer()) * params_.channels(), 0);
  alSourcef(source_, AL_GAIN, static_cast<ALfloat>(volume_));
  return true;
}

void OpenALOutputStream::Start(AudioSourceCallback* callback) {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  DCHECK(callback);
  callback_ = callback;
  primed_ = false;
  // All OpenAL source/buffer traffic happens on the worker thread via Pump().
  worker_.Start(base::BindRepeating(&OpenALOutputStream::Pump,
                                    base::Unretained(this)));
}

void OpenALOutputStream::Stop() {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  // Blocks until any in-flight Pump() invocation has finished.
  worker_.Stop();

  if (source_) {
    // Stopping the source marks all queued buffers as processed so they can be
    // unqueued and recycled on the next Start().
    alSourceStop(source_);
    ALint queued = 0;
    alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
    while (queued-- > 0) {
      ALuint buffer = 0;
      alSourceUnqueueBuffers(source_, 1, &buffer);
    }
  }
  primed_ = false;
  callback_ = nullptr;
}

void OpenALOutputStream::SetVolume(double volume) {
  volume_ = std::clamp(volume, 0.0, 1.0);
  if (source_) {
    alSourcef(source_, AL_GAIN, static_cast<ALfloat>(volume_));
  }
}

void OpenALOutputStream::GetVolume(double* volume) {
  *volume = volume_;
}

void OpenALOutputStream::Close() {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  DCHECK(!callback_);
  if (source_) {
    alDeleteSources(1, &source_);
    source_ = 0;
  }
  if (!buffers_.empty()) {
    alDeleteBuffers(static_cast<ALsizei>(buffers_.size()), buffers_.data());
    buffers_.clear();
  }
  // Note: must be last call, manager may delete |this|.
  manager_->ReleaseOutputStream(this);
}

void OpenALOutputStream::Flush() {}

void OpenALOutputStream::Pump(base::TimeTicks /*ideal_time*/,
                              base::TimeTicks /*now*/) {
  if (!callback_) {
    return;
  }

  if (!primed_) {
    // Fill the whole ring once, then kick off playback.
    for (int i = 0; i < static_cast<int>(buffers_.size()); ++i) {
      FillAndQueue(buffers_[i], /*queued_buffers=*/i);
    }
    alSourcePlay(source_);
    primed_ = true;
    return;
  }

  ALint processed = 0;
  alGetSourcei(source_, AL_BUFFERS_PROCESSED, &processed);
  while (processed-- > 0) {
    ALuint buffer = 0;
    alSourceUnqueueBuffers(source_, 1, &buffer);
    ALint queued = 0;
    alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
    FillAndQueue(buffer, queued);
  }

  // If the source drained (underrun) while we were busy, restart it so the
  // freshly queued buffers play out instead of leaving us silent.
  ALint state = 0;
  alGetSourcei(source_, AL_SOURCE_STATE, &state);
  if (state != AL_PLAYING) {
    ALint queued = 0;
    alGetSourcei(source_, AL_BUFFERS_QUEUED, &queued);
    if (queued > 0) {
      alSourcePlay(source_);
    }
  }
}

void OpenALOutputStream::FillAndQueue(ALuint al_buffer, int queued_buffers) {
  const int frames = params_.frames_per_buffer();
  const base::TimeDelta buffer_duration = base::Microseconds(
      frames * base::Time::kMicrosecondsPerSecond / params_.sample_rate());
  const base::TimeDelta delay = buffer_duration * queued_buffers;

  const int filled =
      callback_->OnMoreData(delay, base::TimeTicks::Now(), {}, audio_bus_.get());
  if (filled < frames) {
    // Zero any frames the source did not provide to avoid replaying stale data.
    audio_bus_->ZeroFramesPartial(filled, frames - filled);
  }

  audio_bus_->ToInterleaved<SignedInt16SampleTypeTraits>(frames,
                                                         interleaved_.data());
  const ALsizei bytes =
      static_cast<ALsizei>(frames) * params_.channels() * sizeof(int16_t);
  alBufferData(al_buffer, al_format_, interleaved_.data(), bytes,
               params_.sample_rate());
  alSourceQueueBuffers(source_, 1, &al_buffer);
}

}  // namespace media
