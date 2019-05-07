/*
 * Copyright (C) 2017 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef ANDROID_HARDWARE_AUDIO_V2_0_UTIL_H
#define ANDROID_HARDWARE_AUDIO_V2_0_UTIL_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/time.h>
#include <stdlib.h>

#include <log/log.h>
#include <cutils/str_parms.h>
#include <cutils/properties.h>

#include <system/audio.h>

#include <tinyalsa/asoundlib.h>
#include <audio_utils/resampler.h>
#include <audio_utils/echo_reference.h>
#include <hardware/audio_effect.h>
#include <audio_effects/effect_aec.h>

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

/* Mixer control names */
#define MIXER_PLAY_VOL              "DVC Out Playback Volume"
#define MIXER_CAPTURE_VOL           "DVC In Capture Volume"

/* GEN3 mixer values */
#define MIXER_PLAY_V_DEFAULT        260000      /* Max volume w/o distorsions */
#define MIXER_PLAY_V_MAX            0x7fffff

#define MIXER_CAPTURE_V_DEFAULT     0x2DC6C0
#define MIXER_CAPTURE_V_MAX         0x7fffff

/* HDMI mixer controls */
#define MIXER_MAXIMUM_LPCM_CHANNELS "Maximum LPCM channels"

/* ALSA cards for GEN3 */
#define CARD_GEN3                 0
#define CARD_GEN3_HDMI            1
#define CARD_GEN3_DEFAULT         CARD_GEN3

/* ALSA ports(devices) for GEN3 */
#define PORT_DAC                    0
#define PORT_HDMI                   0
#define PORT_MIC                    0

/* User serviceable */
/* number of frames per period for HDMI output */
#define HDMI_PERIOD_SIZE            1024
/* number of periods for HDMI output */
#define HDMI_PERIOD_COUNT           2

/* number of frames per short period (low latency) */
#define SHORT_PERIOD_SIZE           1024
/* number of frames per capture period */
#define CAPTURE_PERIOD_SIZE         1024

/* number of pseudo periods for low latency playback */
#define PLAYBACK_SHORT_PERIOD_COUNT 4
/* number of periods for capture */
#define CAPTURE_PERIOD_COUNT        4

#define DEFAULT_OUT_SAMPLING_RATE   48000
#define DEFAULT_IN_SAMPLING_RATE    48000

#define MIN(x, y) ((x) > (y) ? (y) : (x))

#define PTHREAD_MUTEX_LOCK(lock) \
    pthread_mutex_lock(lock);

#define PTHREAD_MUTEX_UNLOCK(lock) \
    pthread_mutex_unlock(lock);

static pcm_config pcm_config_dac = {
    .channels = 2,
    .rate = DEFAULT_OUT_SAMPLING_RATE,
    .period_size = SHORT_PERIOD_SIZE,
    .period_count = PLAYBACK_SHORT_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE
};

static pcm_config pcm_config_hdmi = {
    .channels = 2, /* changed when the stream is opened */
    .rate = DEFAULT_OUT_SAMPLING_RATE, /* changed when the stream is opened */
    .period_size = HDMI_PERIOD_SIZE,
    .period_count = HDMI_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE,
    .start_threshold = 0,
    .avail_min = 0,
};

static pcm_config pcm_config_mic = {
    .channels = 2,
    .rate = DEFAULT_IN_SAMPLING_RATE,
    .period_size = CAPTURE_PERIOD_SIZE,
    .period_count = CAPTURE_PERIOD_COUNT,
    .format = PCM_FORMAT_S16_LE
};

struct route_setting {
    char *ctl_name;
    int intval;
    char *strval;
};

struct mixer_ctls {
    mixer_ctl *play_volume;
};

enum output_type {
    OUTPUT_DEEP_BUF,      // deep PCM buffers output stream
    OUTPUT_LOW_LATENCY,   // low latency output stream
    OUTPUT_HDMI,
    OUTPUT_TOTAL
};

enum pcm_type {
    PCM_NORMAL = 0,
    PCM_HDMI,
    PCM_TOTAL,
};

enum error_types {
    MIXER_FILED
};

#define MAX_PREPROCESSORS 1 /* maximum one AEC per input stream */

inline pcm_config &get_pcm_config_mic () {
    return pcm_config_mic;
}

inline pcm_config &get_cm_config_hdmi () {
    return pcm_config_hdmi;
}

inline pcm_config &get_pcm_config_dac () {
    return pcm_config_dac;
}

/** @return true if gain is between 0 and 1 included. */
constexpr bool isGainNormalized(float gain) {
    return gain >= 0.0 && gain <= 1.0;
}

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_AUDIO_V2_0_UTIL_H
