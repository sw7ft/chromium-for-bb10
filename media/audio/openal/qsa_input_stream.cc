// Copyright 2026 SW7FT. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "media/audio/openal/qsa_input_stream.h"

#include <audio/audio_manager_routing.h>
#include <stdio.h>
#include <string.h>

#include "base/logging.h"
#include "base/time/time.h"
#include "media/audio/audio_manager_base.h"
#include "media/base/audio_bus.h"
#include "media/base/audio_sample_types.h"

namespace media {

QsaInputStream::QsaInputStream(AudioManagerBase* manager,
                               const AudioParameters& params)
    : manager_(manager),
      params_(params),
      audio_bus_(AudioBus::Create(params)) {}

QsaInputStream::~QsaInputStream() {
  DCHECK(!thread_);
}

AudioInputStream::OpenOutcome QsaInputStream::Open() {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  if (!params_.IsValid()) {
    return OpenOutcome::kFailed;
  }

  // Open the capture device. On BB10 the microphone is NOT reachable by opening
  // the raw PCM node directly (snd_pcm_open*/pcmPreferredc all return ENOENT):
  // the audio-manager service has to route a capture source first. The
  // audio_manager_snd_pcm_open_* helpers do that atomically -- they acquire an
  // audio-manager handle of the given type, open the preferred capture PCM, and
  // bind the two together so the route is actually established. We register as
  // VOICE_RECORDING (general app mic capture). Fall back to the by-name helper,
  // then to raw QSA opens for environments without the audio-manager service.
  int card = -1;
  int device = -1;
  pcm_ = nullptr;
  audioman_handle_ = 0;
  int rc = audio_manager_snd_pcm_open_preferred(
      AUDIO_TYPE_VOICE_RECORDING, &pcm_, &audioman_handle_, &card, &device,
      SND_PCM_OPEN_CAPTURE);
  fprintf(stderr,
          "BerryShell: QsaInput am_open_preferred rc=%d card=%d dev=%d "
          "amhandle=%u\n",
          rc, card, device, audioman_handle_);
  if (rc < 0 || !pcm_) {
    pcm_ = nullptr;
    audioman_handle_ = 0;
    rc = audio_manager_snd_pcm_open_name(AUDIO_TYPE_VOICE_RECORDING, &pcm_,
                                         &audioman_handle_,
                                         const_cast<char*>("pcmPreferredc"),
                                         SND_PCM_OPEN_CAPTURE);
    fprintf(stderr, "BerryShell: QsaInput am_open_name rc=%d amhandle=%u\n", rc,
            audioman_handle_);
  }
  if (rc < 0 || !pcm_) {
    pcm_ = nullptr;
    audioman_handle_ = 0;
    rc = snd_pcm_open_preferred(&pcm_, &card, &device, SND_PCM_OPEN_CAPTURE);
    fprintf(stderr, "BerryShell: QsaInput raw open_preferred rc=%d\n", rc);
  }
  if (rc < 0 || !pcm_) {
    LOG(ERROR) << "QsaInputStream: capture open failed: " << snd_strerror(rc);
    pcm_ = nullptr;
    audioman_handle_ = 0;
    // A denied/missing open (e.g. no record_audio permission) is reported as a
    // generic failure so getUserMedia rejects cleanly instead of crashing.
    return OpenOutcome::kFailed;
  }

  fragment_bytes_ = static_cast<size_t>(params_.frames_per_buffer()) *
                    params_.channels() * sizeof(int16_t);

  snd_pcm_channel_params_t p;
  memset(&p, 0, sizeof(p));
  p.mode = SND_PCM_MODE_BLOCK;
  p.channel = SND_PCM_CHANNEL_CAPTURE;
  p.start_mode = SND_PCM_START_DATA;
  p.stop_mode = SND_PCM_STOP_STOP;
  p.format.interleave = 1;
  p.format.format = SND_PCM_SFMT_S16_LE;
  p.format.rate = params_.sample_rate();
  p.format.voices = params_.channels();
  p.buf.block.frag_size = static_cast<int32_t>(fragment_bytes_);
  p.buf.block.frags_min = 1;
  p.buf.block.frags_max = -1;

  rc = snd_pcm_plugin_params(pcm_, &p);
  if (rc < 0) {
    LOG(ERROR) << "QsaInputStream: snd_pcm_plugin_params failed: "
               << snd_strerror(rc);
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    if (audioman_handle_ != 0) {
      audio_manager_free_handle(audioman_handle_);
      audioman_handle_ = 0;
    }
    return OpenOutcome::kFailed;
  }

  if (!Prepare()) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
    if (audioman_handle_ != 0) {
      audio_manager_free_handle(audioman_handle_);
      audioman_handle_ = 0;
    }
    return OpenOutcome::kFailed;
  }

  // Activate the audio-manager handle so the capture route goes live before the
  // first read. Harmless (and skipped) when we fell back to a raw QSA open.
  if (audioman_handle_ != 0) {
    int act = audio_manager_activate_handle(audioman_handle_);
    fprintf(stderr, "BerryShell: QsaInput activate_handle rc=%d\n", act);
  }

  interleaved_.assign(
      static_cast<size_t>(params_.frames_per_buffer()) * params_.channels(), 0);
  return OpenOutcome::kSuccess;
}

bool QsaInputStream::Prepare() {
  int rc = snd_pcm_plugin_prepare(pcm_, SND_PCM_CHANNEL_CAPTURE);
  if (rc < 0) {
    LOG(ERROR) << "QsaInputStream: snd_pcm_plugin_prepare failed: "
               << snd_strerror(rc);
    return false;
  }
  return true;
}

void QsaInputStream::Start(AudioInputCallback* callback) {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  DCHECK(callback);
  if (!pcm_ || thread_) {
    return;
  }
  callback_ = callback;
  running_.store(true, std::memory_order_relaxed);
  // Capture reads block, so service them on a dedicated joinable thread.
  thread_ = std::make_unique<base::DelegateSimpleThread>(this, "QsaCapture");
  thread_->Start();
}

void QsaInputStream::Stop() {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  if (!thread_) {
    return;
  }
  running_.store(false, std::memory_order_relaxed);
  // The read thread wakes at least once per fragment, so Join returns promptly.
  thread_->Join();
  thread_.reset();
  if (pcm_) {
    snd_pcm_plugin_flush(pcm_, SND_PCM_CHANNEL_CAPTURE);
  }
  callback_ = nullptr;
}

void QsaInputStream::Close() {
  DCHECK(manager_->GetTaskRunner()->BelongsToCurrentThread());
  DCHECK(!thread_);
  if (pcm_) {
    snd_pcm_close(pcm_);
    pcm_ = nullptr;
  }
  if (audioman_handle_ != 0) {
    audio_manager_free_handle(audioman_handle_);
    audioman_handle_ = 0;
  }
  // Note: must be last call, manager may delete |this|.
  manager_->ReleaseInputStream(this);
}

double QsaInputStream::GetMaxVolume() {
  return 0.0;
}

void QsaInputStream::SetVolume(double volume) {}

double QsaInputStream::GetVolume() {
  return 0.0;
}

bool QsaInputStream::SetAutomaticGainControl(bool enabled) {
  agc_enabled_ = enabled;
  return true;
}

bool QsaInputStream::GetAutomaticGainControl() {
  return agc_enabled_;
}

bool QsaInputStream::IsMuted() {
  return false;
}

void QsaInputStream::SetOutputDeviceForAec(
    const std::string& output_device_id) {}

void QsaInputStream::Run() {
  const int frames = params_.frames_per_buffer();
  const base::TimeDelta buffer_duration = base::Microseconds(
      frames * base::Time::kMicrosecondsPerSecond / params_.sample_rate());

  while (running_.load(std::memory_order_relaxed)) {
    ssize_t got = snd_pcm_plugin_read(pcm_, interleaved_.data(),
                                      static_cast<size_t>(fragment_bytes_));
    if (got != static_cast<ssize_t>(fragment_bytes_)) {
      // Short read: typically a capture overrun. Inspect status and re-prepare
      // so the stream keeps flowing instead of wedging.
      snd_pcm_channel_status_t status;
      memset(&status, 0, sizeof(status));
      status.channel = SND_PCM_CHANNEL_CAPTURE;
      if (snd_pcm_plugin_status(pcm_, &status) == 0 &&
          (status.status == SND_PCM_STATUS_OVERRUN ||
           status.status == SND_PCM_STATUS_READY)) {
        Prepare();
      }
      if (got <= 0) {
        continue;
      }
      // Zero any frames the device did not provide for this fragment.
      const size_t got_samples = static_cast<size_t>(got) / sizeof(int16_t);
      if (got_samples < interleaved_.size()) {
        memset(interleaved_.data() + got_samples, 0,
               (interleaved_.size() - got_samples) * sizeof(int16_t));
      }
    }

    audio_bus_->FromInterleaved<SignedInt16SampleTypeTraits>(
        interleaved_.data(), frames);
    const base::TimeTicks capture_time =
        base::TimeTicks::Now() - buffer_duration;
    callback_->OnData(audio_bus_.get(), capture_time, /*volume=*/1.0, {});
  }
}

}  // namespace media
