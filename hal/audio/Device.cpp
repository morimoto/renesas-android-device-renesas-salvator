/*
* Copyright (C) 2016 The Android Open Source Project
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

#define LOG_TAG "AudioHAL"
#define LOG_NDEBUG 0

#include <algorithm>
#include <memory.h>
#include <string.h>

#include <log/log.h>

#include "Conversions.h"
#include "Device.h"
#include "StreamIn.h"
#include "StreamOut.h"
#include "Util.h"

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

/* These are values that never change */
route_setting defaults[] = {
    /* general */
    {
        .ctl_name = const_cast<char *>(MIXER_PLAY_VOL),
        .intval = MIXER_PLAY_V_DEFAULT,
    },
    {
        .ctl_name = const_cast<char *>(MIXER_CAPTURE_VOL),
        .intval = MIXER_CAPTURE_V_DEFAULT,
    },
    {
        .ctl_name = NULL,
    },
};

pthread_mutex_t Device::lock;

Device::Device() {
    active_input = NULL;
    for (int i = 0; i < OUTPUT_TOTAL; ++i)
        outputs[i] = NULL;

    this->_mixer = mixer_open(CARD_GEN3_DEFAULT);

    if (!this->_mixer) {
        throw MIXER_FILED;
    }

    this->_mixer_ctls.play_volume = mixer_get_ctl_by_name(this->_mixer,
                                    MIXER_PLAY_VOL);

    if (!this->_mixer_ctls.play_volume) {
        mixer_close(this->_mixer);
        ALOGE("Unable to locate all mixer controls, aborting.");
    }

    /* Set the default route before the PCM stream is opened */
    PTHREAD_MUTEX_LOCK(&this->lock);
    setRouteByArray(this->_mixer, defaults, 1);
    this->mode = AUDIO_MODE_NORMAL;
    this->devices = AUDIO_DEVICE_OUT_SPEAKER | AUDIO_DEVICE_IN_BUILTIN_MIC;
    PTHREAD_MUTEX_UNLOCK(&this->lock);
}

Device::~Device() {
    mixer_close(_mixer);
    delete active_input;
    active_input = NULL;
}

Result Device::analyzeStatus(const char *funcName, int status) {
    if (status != 0) {
        ALOGW("Device %s: %s", funcName, strerror(-status));
    }
    switch (status) {
    case 0:
        return Result::OK;
    case -EINVAL:
        return Result::INVALID_ARGUMENTS;
    case -ENODATA:
        return Result::INVALID_STATE;
    case -ENODEV:
        return Result::NOT_INITIALIZED;
    case -ENOSYS:
        return Result::NOT_SUPPORTED;
    default:
        return Result::INVALID_STATE;
    }
}

void Device::closeInputStream(IStreamIn *stream) {

    if (stream == NULL)
        return;

    stream->standby();
    PTHREAD_MUTEX_LOCK(&this->lock);
    StreamIn *temp = static_cast<StreamIn *>(stream);
    if (temp->read_buf)
        free(temp->read_buf);
    if (temp->resampler) {
        release_resampler(temp->resampler);
    }
    if (temp->proc_buf_in)
        free(temp->proc_buf_in);
    if (temp->proc_buf_out)
        free(temp->proc_buf_out);
    if (temp->ref_buf)
        free(temp->ref_buf);
    temp->read_buf = nullptr;
    temp->resampler = nullptr;
    temp->proc_buf_in = nullptr;
    temp->proc_buf_out = nullptr;
    temp->ref_buf = nullptr;
    PTHREAD_MUTEX_UNLOCK(&this->lock);
}

void Device::closeOutputStream(IStreamOut *stream) {

    if (stream == NULL)
        return;

    ALOGV("%s", __FUNCTION__);
    stream->standby();
    int i;
    PTHREAD_MUTEX_LOCK(&this->lock);
    StreamOut *temp = static_cast<StreamOut *>(stream);
    if (temp->buffer)
        free(temp->buffer);
    if (temp->resampler)
        release_resampler(temp->resampler);
    temp->buffer = nullptr;
    temp->resampler = nullptr;
    for (i = 0; i < OUTPUT_TOTAL; i++) {
        if (this->outputs[i] == stream) {
            this->outputs[i] = NULL;
            break;
        }
    }
    PTHREAD_MUTEX_UNLOCK(&this->lock);
}

char *Device::halGetParameters(const char *keys) {
    ALOGV("%s", __FUNCTION__);
    return strdup("");
}

int Device::halSetParameters(const char *keysAndValues) {
    ALOGV("%s", __FUNCTION__);
    return 0;
}

// Methods from ::android::hardware::audio::V2_0::IDevice follow.
Return<Result> Device::initCheck() {
    ALOGV("%s", __FUNCTION__);
    return Result::OK;
}

Return<Result> Device::setMasterVolume(float volume) {
    ALOGV("%s: volume=%f", __FUNCTION__, volume);
    return Result::NOT_SUPPORTED;
}

Return<void> Device::getMasterVolume(getMasterVolume_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    float volume = 0;
    _hidl_cb(retval, volume);
    return Void();
}

Return<Result> Device::setMicMute(bool mute) {
    ALOGV("%s", __FUNCTION__);
    this->mic_mute = mute;
    return Result::OK;
}

Return<void> Device::getMicMute(getMicMute_cb _hidl_cb) {
    ALOGV("%s", __FUNCTION__);
    Result retval = Result::OK;
    _hidl_cb(retval, this->mic_mute);
    return Void();
}

Return<Result> Device::setMasterMute(bool mute) {
    Result retval(Result::NOT_SUPPORTED);
    return retval;
}

Return<void> Device::getMasterMute(getMasterMute_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    bool mute = false;
    _hidl_cb(retval, mute);
    return Void();
}

Return<void> Device::getInputBufferSize(const AudioConfig &config,
                                        getInputBufferSize_cb _hidl_cb) {
    size_t halBufferSize;

    int channel_count = popcount((unsigned int)config.channelMask);
    if (this->checkInputParameters(config.sampleRateHz, config.format, channel_count) != 0) {
        halBufferSize = 0;
    } else {
        size_t size;

        /* take resampling into account and return the closest majoring
        multiple of 16 frames, as audioflinger expects audio buffers to
        be a multiple of 16 frames */
        size = (pcm_config_mic.period_size * config.sampleRateHz) /
               pcm_config_mic.rate;
        size = ((size + 15) / 16) * 16;

        halBufferSize = size * channel_count * sizeof(short);
    }

    Result retval(Result::INVALID_ARGUMENTS);
    uint64_t bufferSize = 0;
    if (halBufferSize != 0) {
        retval = Result::OK;
        bufferSize = halBufferSize;
    }
    _hidl_cb(retval, bufferSize);
    return Void();
}

Return<void> Device::openOutputStreamLowLatency(int32_t ioHandle,
        const DeviceAddress &device,
        const AudioConfig &config,
        AudioOutputFlag flags,
        openOutputStream_cb _hidl_cb) {
    AudioConfig suggestedConfig;
    ALOGV(
        "open_output_stream handle: %d devices: %x flags: %#x "
        "srate: %d format %#x channels %x address %s",
        ioHandle, static_cast<audio_devices_t>(device.device),
        static_cast<audio_output_flags_t>(flags), config.sampleRateHz,
        config.format, config.channelMask,
        deviceAddressToHal(device).c_str());
    StreamOut *streamOut = new StreamOut(this, OUTPUT_LOW_LATENCY);
    Result status;
    int output_type;

    streamOut->channel_mask = AUDIO_CHANNEL_OUT_STEREO;

    if (this->outputs[OUTPUT_LOW_LATENCY] != NULL) {
        status = Result::NOT_SUPPORTED;
    } else {
        output_type = OUTPUT_LOW_LATENCY;

        status = analyzeStatus("openOutputStream", create_resampler(DEFAULT_OUT_SAMPLING_RATE,
                               DEFAULT_OUT_SAMPLING_RATE,
                               2,
                               RESAMPLER_QUALITY_DEFAULT,
                               NULL,
                               &streamOut->resampler));
        if (status == Result::OK) {

            streamOut->_standby = 1;

            /* FIXME: when we support multiple output devices, we will want to
             * do the following:
             * adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
             * adev->devices |= out->device;
             * select_output_device(adev);
             * This is because out_set_parameters() with a route is not
             * guaranteed to be called after an output stream is opened. */

            suggestedConfig.format = AudioFormat(AUDIO_FORMAT_PCM_16_BIT);
            suggestedConfig.channelMask = AudioChannelMask(AUDIO_CHANNEL_OUT_STEREO);
            suggestedConfig.sampleRateHz = DEFAULT_OUT_SAMPLING_RATE;
            this->outputs[output_type] = streamOut;

            streamOut->requested_rate = config.sampleRateHz;
            streamOut->requested_channel_mask = static_cast<audio_channel_mask_t>(config.channelMask);
        }
    }

    ALOGV("open_output_stream status %d", status);
    _hidl_cb(status, streamOut, suggestedConfig);
    return Void();
}

Return<void> Device::openOutputStreamHdmi(int32_t ioHandle,
        const DeviceAddress &device,
        const AudioConfig &config,
        AudioOutputFlag flags,
        openOutputStream_cb _hidl_cb) {
    AudioConfig suggestedConfig;
    ALOGV(
        "open_output_stream handle: %d devices: %x flags: %#x "
        "srate: %d format %#x channels %x address %s",
        ioHandle, static_cast<audio_devices_t>(device.device),
        static_cast<audio_output_flags_t>(flags), config.sampleRateHz,
        config.format, config.channelMask,
        deviceAddressToHal(device).c_str());
    StreamOut *streamOut = new StreamOut(this, OUTPUT_HDMI);
    Result status;
    int output_type;

    streamOut->channel_mask = AUDIO_CHANNEL_OUT_STEREO;

    if (this->outputs[OUTPUT_HDMI] != NULL) {
        status = Result::NOT_SUPPORTED;
    } else {
        output_type = OUTPUT_HDMI;
        streamOut->config[PCM_HDMI] = get_cm_config_hdmi();
        streamOut->config[PCM_HDMI].rate = config.sampleRateHz;
        streamOut->config[PCM_HDMI].channels = popcount(static_cast<audio_channel_mask_t>
                                               (config.channelMask));
        /* FIXME: workaround for channel swap on first playback after opening the output */
        streamOut->restart_periods_cnt = streamOut->config[PCM_HDMI].period_count * 2;

        status = analyzeStatus("openOutputStream", create_resampler(DEFAULT_OUT_SAMPLING_RATE,
                               DEFAULT_OUT_SAMPLING_RATE,
                               2,
                               RESAMPLER_QUALITY_DEFAULT,
                               NULL,
                               &streamOut->resampler));
        if (status == Result::OK) {

            streamOut->_standby = 1;

            /* FIXME: when we support multiple output devices, we will want to
             * do the following:
             * adev->devices &= ~AUDIO_DEVICE_OUT_ALL;
             * adev->devices |= out->device;
             * select_output_device(adev);
             * This is because out_set_parameters() with a route is not
             * guaranteed to be called after an output stream is opened. */

            suggestedConfig.format = AudioFormat(AUDIO_FORMAT_PCM_16_BIT);
            suggestedConfig.channelMask = AudioChannelMask(AUDIO_CHANNEL_OUT_STEREO);
            suggestedConfig.sampleRateHz = DEFAULT_OUT_SAMPLING_RATE;
            this->outputs[output_type] = streamOut;

            streamOut->requested_rate = config.sampleRateHz;
            streamOut->requested_channel_mask = static_cast<audio_channel_mask_t>(config.channelMask);
        }
    }

    ALOGV("open_output_stream status %d", status);
    _hidl_cb(status, streamOut, suggestedConfig);
    return Void();
}

Return<void> Device::openOutputStream(int32_t ioHandle,
                                      const DeviceAddress &device,
                                      const AudioConfig &config,
                                      AudioOutputFlag flags,
                                      openOutputStream_cb _hidl_cb) {
    if (devices == AUDIO_DEVICE_OUT_AUX_DIGITAL)
        return openOutputStreamHdmi(ioHandle, device, config, flags, _hidl_cb);
    else
        return openOutputStreamLowLatency(ioHandle, device, config, flags, _hidl_cb);
}

Return<void> Device::openInputStream(int32_t ioHandle,
                                     const DeviceAddress &device,
                                     const AudioConfig &config,
                                     AudioInputFlag flags, AudioSource source,
                                     openInputStream_cb _hidl_cb) {
    AudioConfig suggestedConfig = config;
    ALOGE(
        "open_input_stream handle: %d devices: %x flags: %#x "
        "srate: %d format %#x channels %x address %s source %d",
        ioHandle, static_cast<audio_devices_t>(device.device),
        static_cast<audio_input_flags_t>(flags), suggestedConfig.sampleRateHz,
        suggestedConfig.format, suggestedConfig.channelMask,
        deviceAddressToHal(device).c_str(),
        static_cast<audio_source_t>(source));

    IStreamIn *streamIn = new StreamIn(this);
    Result status = Result::OK;

    int channel_count = popcount(static_cast<audio_channel_mask_t>(suggestedConfig.channelMask));
    StreamIn *stream_in = static_cast<StreamIn *>(streamIn);
    ALOGV("%s: req_rate = %d, format = %d, ch = %d", __FUNCTION__,
          suggestedConfig.sampleRateHz, suggestedConfig.format, channel_count);

    if (checkInputParameters(suggestedConfig.sampleRateHz, suggestedConfig.format, channel_count) != 0) {
        status = Result::INVALID_ARGUMENTS;
    } else {
        stream_in->requested_rate = suggestedConfig.sampleRateHz;

        memcpy(&stream_in->config, &get_pcm_config_mic(), sizeof(pcm_config));
        stream_in->config.channels = channel_count;

        stream_in->main_channels = static_cast<audio_channel_mask_t>(suggestedConfig.channelMask);
        stream_in->aux_channels = stream_in->main_channels;

        /* initialisation of preprocessor structure array is implicit with the calloc.
         * same for in->aux_channels and in->aux_channels_changed */
        if (stream_in->requested_rate != stream_in->config.rate) {
            stream_in->buf_provider.get_next_buffer = StreamIn::getNextBuffer;
            stream_in->buf_provider.release_buffer = StreamIn::releaseBuffer;

            status = analyzeStatus("openInputStream", create_resampler(stream_in->config.rate,
                                   stream_in->requested_rate,
                                   stream_in->config.channels,
                                   RESAMPLER_QUALITY_DEFAULT,
                                   &stream_in->buf_provider,
                                   &stream_in->resampler));

            if (status != Result::OK) {
                status = Result::INVALID_ARGUMENTS;
                if (stream_in->resampler)
                    release_resampler(stream_in->resampler);
                closeInputStream(stream_in);
            }
        }

        if (status == Result::OK) {
            stream_in->_standby = 1;
            stream_in->device = devices;

        }
    }

    ALOGV("open_input_stream status %d", status);
    _hidl_cb(status, streamIn, suggestedConfig);
    return Void();
}

Return<bool> Device::supportsAudioPatches() {
    return false;
}

Return<void> Device::createAudioPatch(const hidl_vec<AudioPortConfig> &sources,
                                      const hidl_vec<AudioPortConfig> &sinks,
                                      createAudioPatch_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    AudioPatchHandle patch = 0;
    _hidl_cb(retval, patch);
    return Void();
}

Return<Result> Device::releaseAudioPatch(int32_t patch) {
    return Result::NOT_SUPPORTED;
}

Return<void> Device::getAudioPort(const AudioPort &port,
                                  getAudioPort_cb _hidl_cb) {
    Result retval = Result::NOT_SUPPORTED;
    AudioPort resultPort = port;
    _hidl_cb(retval, resultPort);
    return Void();
}

Return<Result> Device::setAudioPortConfig(const AudioPortConfig &config) {
    return Result::NOT_SUPPORTED;
}

Return<AudioHwSync> Device::getHwAvSync() {
    int halHwAvSync;
    Result retval = getParam(AudioParameter::keyHwAvSync, &halHwAvSync);
    return retval == Result::OK ? halHwAvSync : AUDIO_HW_SYNC_INVALID;
}

Return<Result> Device::setScreenState(bool turnedOn) {
    return setParam(AudioParameter::keyScreenState, turnedOn);
}

Return<void> Device::getParameters(const hidl_vec<hidl_string> &keys,
                                   getParameters_cb _hidl_cb) {
    getParametersImpl(keys, _hidl_cb);
    return Void();
}

Return<Result> Device::setParameters(
    const hidl_vec<ParameterValue> &parameters) {
    return setParametersImpl(parameters);
}

Return<void> Device::debugDump(const hidl_handle &fd) {
    ALOGV("%s", __FUNCTION__);
    return Void();
}

int Device::setVoiceVolume(float /*volume*/) {
    ALOGV("%s", __FUNCTION__);
    return 0;
}

int Device::setMode(audio_mode_t /*mode*/) {
    ALOGV("%s", __FUNCTION__);
    return 0;
}

int Device::checkInputParameters(uint32_t sampleRateHz, AudioFormat format, int channel_count) {
    if (format != AudioFormat::PCM_16_BIT)
        return -EINVAL;

    if ((channel_count < 1) || (channel_count > 2))
        return -EINVAL;

    switch (sampleRateHz) {
    case 8000:
    case 11025:
    case 16000:
    case 22050:
    case 24000:
    case 32000:
    case 44100:
    case 48000:
        break;
    default:
        return -EINVAL;
    }

    return 0;
}

int Device::setRouteByArray(mixer *_mixer, route_setting *route,
                            int enable) {
    struct mixer_ctl *ctl;
    unsigned int i, j;

    /* Go through the route array and set each value */
    i = 0;
    while (route[i].ctl_name) {
        ctl = mixer_get_ctl_by_name(_mixer, route[i].ctl_name);
        if (!ctl)
            return -EINVAL;

        if (route[i].strval) {
            if (enable)
                mixer_ctl_set_enum_by_string(ctl, route[i].strval);
            else
                mixer_ctl_set_enum_by_string(ctl, "Off");
        } else {
            /* This ensures multiple (i.e. stereo) values are set jointly */
            for (j = 0; j < mixer_ctl_get_num_values(ctl); j++) {
                if (enable)
                    mixer_ctl_set_value(ctl, j, route[i].intval);
                else
                    mixer_ctl_set_value(ctl, j, 0);
            }
        }
        i++;
    }

    return 0;
}

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android
