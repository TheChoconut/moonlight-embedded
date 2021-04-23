/*
  * This file is part of Moonlight Embedded.
  *
  * Copyright (C) 2015-2017 Iwan Timmer
  * Copyright (C) 2016 OtherCrashOverride, Daniel Mehrwald
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

#include <Limelight.h>

#include <sys/utsname.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <codec.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include "../logging.h"
#include "../platform.h"

#define EXTERNAL_PTS 0x01
#define SYNC_OUTSIDE 0x02
#define UCODE_IP_ONLY_PARAM 0x08
#define MAX_ERR_ATTEMPTS 20
#define DECODER_BUFFER_SIZE 300*1024
#define DECODER_BUFFER_SIZE_REDUCED 150*1024
#define DR_VIDEO_DELAY -2

static codec_para_t codecParam = { 0 };
static char* frame_buffer;

time_t lastMeasuredTime;
int lastFrameNumber = -1, droppedFrames = 0, totalFrames = 0, nfis = 0, hFPS = -1, lFPS = 1000, avgFPS = -1, decodeTime = -1, avgDec = -1;

int LASTVF = 0, LASTWIDTH = 0, LASTHEIGHT = 0, LASTRR = 0, video_delay = -1;
bool optimizedBuffer = false;
void aml_set_video_init_delay(int delaySec) {
  video_delay = delaySec;
}
void aml_use_optimized_fb_algorithm() {
  optimizedBuffer = true;
  _moonlight_log(WARN, "Amlogic decoder will use optimized algorithm. You may experience higher latency.\n");
}

int aml_setup(int videoFormat, int width, int height, int redrawRate, void* context, int drFlags) {
  codecParam.stream_type = STREAM_TYPE_ES_VIDEO;
  codecParam.has_video = 1;
  codecParam.noblock = 0;
  codecParam.am_sysinfo.param = 0;

  if (LASTWIDTH == 0) {
    LASTVF = videoFormat;
    LASTWIDTH = width;
    LASTHEIGHT = height;
    LASTRR = redrawRate; 
  }

  if (video_delay >= 0)
    return 0;
  
  switch (videoFormat) {
    case VIDEO_FORMAT_H264:
      if (width > 1920 || height > 1080) {
        codecParam.video_type = VFORMAT_H264_4K2K;
        codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_H264_4K2K;
      } else {
        codecParam.video_type = VFORMAT_H264;
        codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_H264;
        
        // Workaround for decoding special case of C1, 1080p, H264
        int major, minor;
        struct utsname name;
        uname(&name);
        int ret = sscanf(name.release, "%d.%d", &major, &minor);
        if (!(major > 3 || (major == 3 && minor >= 14)) && width == 1920 && height == 1080)
          codecParam.am_sysinfo.param = (void*) UCODE_IP_ONLY_PARAM;
      }
      break;
    case VIDEO_FORMAT_H265:
      codecParam.video_type = VFORMAT_HEVC;
      codecParam.am_sysinfo.format = VIDEO_DEC_FORMAT_HEVC;
      break;
    default:
      _moonlight_log(ERR, "Video format not supported\n");
      return -1;
  }
    
  codecParam.am_sysinfo.width = width;
  codecParam.am_sysinfo.height = height;
  codecParam.am_sysinfo.rate = 96000 / redrawRate;
  codecParam.am_sysinfo.param = (void*) ((size_t) codecParam.am_sysinfo.param | EXTERNAL_PTS | SYNC_OUTSIDE); // combine this with SYNC_OUTSIDE
  
  int ret;
  if ((ret = codec_init(&codecParam)) != 0) {
    _moonlight_log(ERR, "codec_init error: %x\n", ret);
    return -2;
  }
  
  if ((ret = codec_set_freerun_mode(&codecParam, 1)) != 0) {
    _moonlight_log(ERR, "Can't set Freerun mode: %x\n", ret);
    return -2;
  }
  if (frame_buffer == NULL && optimizedBuffer) {
    frame_buffer = malloc(DECODER_BUFFER_SIZE);
    if (frame_buffer == NULL)
      frame_buffer = malloc(DECODER_BUFFER_SIZE_REDUCED);
  }

  if (frame_buffer == NULL && optimizedBuffer) {
    _moonlight_log(ERR, "Not enough memory to initialize frame buffer\n");
    return -2;
  }
  return 0;
}

void aml_cleanup() {
  codec_close(&codecParam);

  if (frame_buffer != NULL)
    free(frame_buffer);

  // HACK: Write amlogic decoder stats here.

  FILE* stats = fopen("aml_decoder.stats", "w");
  int stream_status = 1;
  if (lFPS == 1000 || avgFPS == -1) {
    lFPS = -1;
    stream_status = -1;
  }
  fprintf(stats, "StreamStatus = %i\n", stream_status);
  fprintf(stats, "AverageFPS = %i\n", avgFPS);
  fprintf(stats, "LowestFPS = %i\n", lFPS);
  fprintf(stats, "HighestFPS = %i\n", hFPS);
  fprintf(stats, "NetworkDroppedFrames = %i\n", droppedFrames);
  fprintf(stats, "AvgDecodingTime = %d us", avgDec);
  fclose(stats);
}

struct timespec start, lastMeasure, end;
int check_for_video_delay() {
  if (video_delay > 0) {
    if (lastMeasure.tv_nsec == 0) {
      _moonlight_log(WARN, "Video system was delayed for %d seconds and not initialized...\n", video_delay);
      clock_gettime(CLOCK_MONOTONIC_RAW, &lastMeasure);
    } else if (start.tv_sec - lastMeasure.tv_sec >= video_delay) {
      video_delay = -1;
      _moonlight_log(INFO, "Initializing AML video after delay...\n");
      if (aml_setup(LASTVF, LASTWIDTH, LASTHEIGHT, LASTRR, NULL, 0) < 0) {
        _moonlight_log(ERR, "Video failed to initialize after delay... Closing the program.\n");
        platform_stop(AML);
        exit(0);
        return DR_VIDEO_DELAY;
      }
      platform_start(AML);
      _moonlight_log(INFO, "Video initialized sucessfully. Requesting IDR frame...\n");
      return DR_NEED_IDR;
    }
    return DR_VIDEO_DELAY;
  }
  return DR_OK;
}

int aml_submit_decode_unit(PDECODE_UNIT decodeUnit) {
  clock_gettime(CLOCK_MONOTONIC_RAW, &start);
  int result = check_for_video_delay(), api, length = 0;

  if (result != DR_OK) {  
    lastFrameNumber = decodeUnit->frameNumber;
    return result == DR_VIDEO_DELAY ? DR_OK : result;
  }

  codec_checkin_pts(&codecParam, decodeUnit->presentationTimeMs);
  if (lastMeasure.tv_nsec == 0 || (start.tv_sec - lastMeasure.tv_sec) >= 1) {
    if (nfis > 0) {
      avgFPS = nfis / (start.tv_sec - lastMeasure.tv_sec);
      avgDec = decodeTime / nfis;
      if (nfis < lFPS)
        lFPS = nfis;
      if (nfis > hFPS)
        hFPS = nfis;
    }
    nfis = 0;
    decodeTime = 0;
    clock_gettime(CLOCK_MONOTONIC_RAW, &lastMeasure);
  }
  
  if (decodeUnit->frameNumber != lastFrameNumber && decodeUnit->frameNumber != lastFrameNumber+1) {
    int framesDropped = decodeUnit->frameNumber - lastFrameNumber - 1;
    droppedFrames += framesDropped;
    _moonlight_log(WARN,"Dropped %d frames!\n", framesDropped);
  }
  lastFrameNumber = decodeUnit->frameNumber;
  totalFrames++;
  nfis++;

  if (optimizedBuffer) {
    
    if (decodeUnit->fullLength > 0 && decodeUnit->fullLength < DECODER_BUFFER_SIZE) {
          PLENTRY entry = decodeUnit->bufferList;
          while (entry != NULL) {
            memcpy(frame_buffer+length, entry->data, entry->length);
            length += entry->length;
            entry = entry->next;
          }
          int errCounter = 0;
          while (errCounter < MAX_ERR_ATTEMPTS) {
            api = codec_write(&codecParam, frame_buffer, length);
            if (api < 0) {
              if (errno != EAGAIN) {
                _moonlight_log(ERR, "codec_write error: %x %d\n", api, errno);
                droppedFrames += 1;
                codec_reset(&codecParam);
                result = DR_NEED_IDR;
                break;
              } else {
                _moonlight_log(ERR, "EAGAIN triggered, trying again...\n");
                usleep(20000);
                errCounter++;
                continue;
              }
            }
            break;
          }
    } else {
      _moonlight_log(ERR, "Video decode buffer too small, %i > %i\n", decodeUnit->fullLength, DECODER_BUFFER_SIZE);
      platform_stop(AML);
      exit(1);
    }
  } else {
    PLENTRY entry = decodeUnit->bufferList;
    int errCounter = 0;
    while (entry != NULL && errCounter < MAX_ERR_ATTEMPTS) {
      api = codec_write(&codecParam, entry->data, entry->length);
      if (api < 0) {
        if (errno != EAGAIN) {
          _moonlight_log(ERR, "codec_write error: %x %d\n", api, errno);
          droppedFrames += 1;
          codec_reset(&codecParam);
          result = DR_NEED_IDR;
          break;
        } else {
          _moonlight_log(ERR, "EAGAIN triggered, trying again...\n");
          usleep(20000);
          errCounter++;
          continue;
        }
      }
      entry = entry->next;
    }
  }
  clock_gettime(CLOCK_MONOTONIC_RAW, &end);
  decodeTime += (end.tv_sec - start.tv_sec) * 1000000 + (end.tv_nsec - start.tv_nsec) / 1000;
  return result;
}

DECODER_RENDERER_CALLBACKS decoder_callbacks_aml = {
  .setup = aml_setup,
  .cleanup = aml_cleanup,
  .submitDecodeUnit = aml_submit_decode_unit,
  .capabilities = CAPABILITY_DIRECT_SUBMIT | CAPABILITY_SLICES_PER_FRAME(8),
};