// Copyright 2026 SW7FT. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef MEDIA_AUDIO_OPENAL_QSA_INPUT_STREAM_H_
#define MEDIA_AUDIO_OPENAL_QSA_INPUT_STREAM_H_

#include <stdint.h>
#include <sys/asoundlib.h>

#include <atomic>
#include <memory>
#include <vector>

#include "base/threading/simple_thread.h"
#include "media/audio/audio_io.h"
#include "media/base/audio_parameters.h"

namespace media {

class AudioBus;
class AudioManagerBase;

// AudioInputStream backed by QSA (QNX Sound Architecture), the native capture
// API on QNX/BB10. OpenAL on BB10 has no capture extension, so the microphone
// is read through libasound's PCM plugin layer (snd_pcm_plugin_read), which
// also resamples/reformats to the requested rate/format. Capture reads are
// blocking, so they run on a dedicated thread that converts each fragment to an
// AudioBus and forwards it to the AudioInputCallback.
//
// Note: on-device this only works when the host app holds the BB10
// "record_audio" permission; otherwise snd_pcm_open_preferred is denied and
// Open() fails (getUserMedia then rejects, rather than crashing).
class QsaInputStream : public AudioInputStream,
                       public base::DelegateSimpleThread::Delegate {
 public:
  QsaInputStream(AudioManagerBase* manager, const AudioParameters& params);

  QsaInputStream(const QsaInputStream&) = delete;
  QsaInputStream& operator=(const QsaInputStream&) = delete;

  ~QsaInputStream() override;

  // AudioInputStream:
  OpenOutcome Open() override;
  void Start(AudioInputCallback* callback) override;
  void Stop() override;
  void Close() override;
  double GetMaxVolume() override;
  void SetVolume(double volume) override;
  double GetVolume() override;
  bool SetAutomaticGainControl(bool enabled) override;
  bool GetAutomaticGainControl() override;
  bool IsMuted() override;
  void SetOutputDeviceForAec(const std::string& output_device_id) override;

  // base::DelegateSimpleThread::Delegate:
  void Run() override;

 private:
  // Reconfigures the capture channel after an overrun/error.
  bool Prepare();

  AudioManagerBase* const manager_;
  const AudioParameters params_;

  snd_pcm_t* pcm_ = nullptr;
  // BB10 audio-manager handle bound to |pcm_| (acquired via the
  // audio_manager_snd_pcm_open_* helpers). Required for the capture route to
  // actually open; 0 means "none". Freed in Close().
  unsigned int audioman_handle_ = 0;
  AudioInputCallback* callback_ = nullptr;  // Not owned.
  std::unique_ptr<AudioBus> audio_bus_;
  std::vector<int16_t> interleaved_;
  size_t fragment_bytes_ = 0;

  std::atomic<bool> running_{false};
  std::unique_ptr<base::DelegateSimpleThread> thread_;
  bool agc_enabled_ = false;
};

}  // namespace media

#endif  // MEDIA_AUDIO_OPENAL_QSA_INPUT_STREAM_H_
