/*
 * This file is part of Moonlight Embedded.
 *
 * Copyright (C) 2015-2017 Iwan Timmer
 *
 * Moonlight is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * Moonlight is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Moonlight; if not, see <http://www.gnu.org/licenses/>.
 */

#include "audio.h"

#include <stdio.h>
#include "../logging.h"
#include <opus_multistream.h>
#include <alsa/asoundlib.h>

#define CHECK_RETURN(f) if ((rc = f) < 0) { _moonlight_log(ERR, "Alsa error code %d\n", rc); return -1; }

static snd_pcm_t *handle;
static OpusMSDecoder* decoder;
static short pcmBuffer[FRAME_SIZE * MAX_CHANNEL_COUNT];

int initAudioConfig = -1, audio_delay = 0;
OPUS_MULTISTREAM_CONFIGURATION initOpusConfig;
void* initContext;

void alsa_set_audio_init_delay(int delaySec) {
  audio_delay = delaySec;
}

static int alsa_renderer_init(int audioConfiguration, POPUS_MULTISTREAM_CONFIGURATION opusConfig, void* context, int arFlags) {
  int rc;
  unsigned char alsaMapping[MAX_CHANNEL_COUNT];

  if (initAudioConfig == -1 && audio_delay > 0) {
    initAudioConfig = audioConfiguration;
    initOpusConfig = *opusConfig;
    initContext = context;
    return 0;
  }

  /* The supplied mapping array has order: FL-FR-C-LFE-RL-RR
   * ALSA expects the order: FL-FR-RL-RR-C-LFE
   * We need copy the mapping locally and swap the channels around.
   */
  alsaMapping[0] = opusConfig->mapping[0];
  alsaMapping[1] = opusConfig->mapping[1];
  if (opusConfig->channelCount == 6) {
    alsaMapping[2] = opusConfig->mapping[4];
    alsaMapping[3] = opusConfig->mapping[5];
    alsaMapping[4] = opusConfig->mapping[2];
    alsaMapping[5] = opusConfig->mapping[3];
  }


  int channelCount = opusConfig->channelCount;

  decoder = opus_multistream_decoder_create(opusConfig->sampleRate, channelCount, opusConfig->streams, opusConfig->coupledStreams, alsaMapping, &rc);

  snd_pcm_hw_params_t *hw_params;
  snd_pcm_sw_params_t *sw_params;
  snd_pcm_uframes_t period_size = FRAME_SIZE * FRAME_BUFFER;
  snd_pcm_uframes_t buffer_size = 2 * period_size;
  unsigned int sampleRate = opusConfig->sampleRate;

  char* audio_device = (char*) context;
  if (audio_device == NULL)
    audio_device = "sysdefault";

  /* Open PCM device for playback. */
  CHECK_RETURN(snd_pcm_open(&handle, audio_device, SND_PCM_STREAM_PLAYBACK, SND_PCM_NONBLOCK))

  /* Set hardware parameters */
  _moonlight_log(INFO, "Allocating alsa hardware parameters...\n");
  CHECK_RETURN(snd_pcm_hw_params_malloc(&hw_params));
  _moonlight_log(DEBUG, "Initialize HW parameters...\n");
  CHECK_RETURN(snd_pcm_hw_params_any(handle, hw_params));
  _moonlight_log(DEBUG, "Set access to interleaved...\n");
  CHECK_RETURN(snd_pcm_hw_params_set_access(handle, hw_params, SND_PCM_ACCESS_RW_INTERLEAVED));
  _moonlight_log(DEBUG, "Set format to S16_LE...\n");
  CHECK_RETURN(snd_pcm_hw_params_set_format(handle, hw_params, SND_PCM_FORMAT_S16_LE));
  _moonlight_log(DEBUG, "Set rate near %u...\n", &sampleRate);
  CHECK_RETURN(snd_pcm_hw_params_set_rate_near(handle, hw_params, &sampleRate, NULL));
  _moonlight_log(DEBUG, "Set channel count to %d...\n", channelCount);
  CHECK_RETURN(snd_pcm_hw_params_set_channels_near(handle, hw_params, &channelCount));
  if (opusConfig->channelCount > channelCount) {
    printf("Failed to assign enough channels, only %d available (%d required).\n", channelCount, opusConfig->channelCount);
    return -1;
  }

  _moonlight_log(DEBUG, "Set period size to %d...\n", &period_size);
  CHECK_RETURN(snd_pcm_hw_params_set_period_size_near(handle, hw_params, &period_size, NULL));
  _moonlight_log(DEBUG, "Set buffer size to %d...\n", &buffer_size);
  CHECK_RETURN(snd_pcm_hw_params_set_buffer_size_near(handle, hw_params, &buffer_size));
  _moonlight_log(DEBUG, "Setting new HW params %d...\n");
  CHECK_RETURN(snd_pcm_hw_params(handle, hw_params));
  snd_pcm_hw_params_free(hw_params);

  /* Set software parameters */
  _moonlight_log(DEBUG, "Allocating new SW params...\n");
  CHECK_RETURN(snd_pcm_sw_params_malloc(&sw_params));
  _moonlight_log(DEBUG, "Initialize new SW params...\n");
  CHECK_RETURN(snd_pcm_sw_params_current(handle, sw_params));
  _moonlight_log(DEBUG, "Setting SW avail mid to %d...\n", period_size);
  CHECK_RETURN(snd_pcm_sw_params_set_avail_min(handle, sw_params, period_size));
  _moonlight_log(DEBUG, "Setting SW start thresh to %d...\n", period_size);
  CHECK_RETURN(snd_pcm_sw_params_set_start_threshold(handle, sw_params, buffer_size));
  _moonlight_log(DEBUG, "Setting new SW paremters...\n");
  CHECK_RETURN(snd_pcm_sw_params(handle, sw_params));
  snd_pcm_sw_params_free(sw_params);

  _moonlight_log(DEBUG, "snd_pcm_prepare()...\n");
  CHECK_RETURN(snd_pcm_prepare(handle));

  audio_delay = 0;
  return 0;
}

static void alsa_renderer_cleanup() {
  if (decoder != NULL)
    opus_multistream_decoder_destroy(decoder);

  if (handle != NULL) {
    snd_pcm_drain(handle);
    snd_pcm_close(handle);
  }
}

struct timespec currentTime, lastTry;
static bool check_for_audio_delay() {
  if (audio_delay > 0) {
    clock_gettime(CLOCK_MONOTONIC_RAW, &currentTime);

    if (lastTry.tv_nsec == 0) {
      _moonlight_log(WARN, "Audio was delayed for %d seconds and not initialized...\n", audio_delay);
      clock_gettime(CLOCK_MONOTONIC_RAW, &lastTry);
    } else if (currentTime.tv_sec - lastTry.tv_sec >= audio_delay)
      audio_delay = -4;
    
    return false;
  } else if (audio_delay < 0) {
    
    clock_gettime(CLOCK_MONOTONIC_RAW, &currentTime);
    if (audio_delay < -1) {
      if (currentTime.tv_sec - lastTry.tv_sec >= 1 || audio_delay == -4) {
        audio_delay++;
        _moonlight_log(WARN, "Trying to initialize audio after delay (try=%d)...\n", audio_delay);
        alsa_renderer_init(initAudioConfig, &initOpusConfig, initContext, 0);
        // alsa_renderer_init() sets audio_delay to 0 on success.
        if (audio_delay < 0) {
          _moonlight_log(ERR, "Audio initializing failed... Retrying later... (try=%d)...\n", audio_delay);
          clock_gettime(CLOCK_MONOTONIC_RAW, &lastTry);
        }
          
      }
    } else {
      _moonlight_log(ERR, "Failed to initialize audio after delay...\n");
      exit(0);
    }
    return false;
  }
  return true;
}

static void alsa_renderer_decode_and_play_sample(char* data, int length) {
  if (check_for_audio_delay() == false) return;

  int decodeLen = opus_multistream_decode(decoder, data, length, pcmBuffer, FRAME_SIZE, 0);
  if (decodeLen > 0) {
    int rc = snd_pcm_writei(handle, pcmBuffer, decodeLen);
    if (rc == -EPIPE)
      snd_pcm_recover(handle, rc, 1);

    if (rc<0)
      _moonlight_log(ERR, "Alsa error from writei: %d\n", rc);
    else if (decodeLen != rc)
      _moonlight_log(WARN,"Alsa shortm write, write %d frames\n", rc);
  } else {
    _moonlight_log(ERR, "Opus error from decode: %d\n", decodeLen);
  }
}

AUDIO_RENDERER_CALLBACKS audio_callbacks_alsa = {
  .init = alsa_renderer_init,
  .cleanup = alsa_renderer_cleanup,
  .decodeAndPlaySample = alsa_renderer_decode_and_play_sample,
  .capabilities = CAPABILITY_DIRECT_SUBMIT,
};
