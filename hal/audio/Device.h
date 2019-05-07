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

#ifndef ANDROID_HARDWARE_AUDIO_V2_0_DEVICE_H
#define ANDROID_HARDWARE_AUDIO_V2_0_DEVICE_H

#include <memory>

#include <media/AudioParameter.h>
#include <android/hardware/audio/2.0/IDevice.h>
#include <hidl/Status.h>

#include <hidl/MQDescriptor.h>

#include "ParametersUtil.h"
#include "Util.h"


namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

using ::android::hardware::audio::common::V2_0::AudioConfig;
using ::android::hardware::audio::common::V2_0::AudioHwSync;
using ::android::hardware::audio::common::V2_0::AudioInputFlag;
using ::android::hardware::audio::common::V2_0::AudioOutputFlag;
using ::android::hardware::audio::common::V2_0::AudioPatchHandle;
using ::android::hardware::audio::common::V2_0::AudioPort;
using ::android::hardware::audio::common::V2_0::AudioPortConfig;
using ::android::hardware::audio::common::V2_0::AudioSource;
using ::android::hardware::audio::common::V2_0::AudioFormat;
using ::android::hardware::audio::V2_0::DeviceAddress;
using ::android::hardware::audio::V2_0::IDevice;
using ::android::hardware::audio::V2_0::IStreamIn;
using ::android::hardware::audio::V2_0::IStreamOut;
using ::android::hardware::audio::V2_0::ParameterValue;
using ::android::hardware::audio::V2_0::Result;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;
using ::android::sp;

struct Device : public IDevice, public ParametersUtil {
    explicit Device();

    // Methods from ::android::hardware::audio::V2_0::IDevice follow.
    Return<Result> initCheck()  override;
    Return<Result> setMasterVolume(float volume)  override;
    Return<void> getMasterVolume(getMasterVolume_cb _hidl_cb)  override;
    Return<Result> setMicMute(bool mute)  override;
    Return<void> getMicMute(getMicMute_cb _hidl_cb)  override;
    Return<Result> setMasterMute(bool mute)  override;
    Return<void> getMasterMute(getMasterMute_cb _hidl_cb)  override;
    Return<void> getInputBufferSize(
        const AudioConfig &config, getInputBufferSize_cb _hidl_cb)  override;
    Return<void> openOutputStream(
        int32_t ioHandle,
        const DeviceAddress &device,
        const AudioConfig &config,
        AudioOutputFlag flags,
        openOutputStream_cb _hidl_cb)  override;
    Return<void> openInputStream(
        int32_t ioHandle,
        const DeviceAddress &device,
        const AudioConfig &config,
        AudioInputFlag flags,
        AudioSource source,
        openInputStream_cb _hidl_cb)  override;
    Return<bool> supportsAudioPatches()  override;
    Return<void> createAudioPatch(
        const hidl_vec<AudioPortConfig> &sources,
        const hidl_vec<AudioPortConfig> &sinks,
        createAudioPatch_cb _hidl_cb)  override;
    Return<Result> releaseAudioPatch(int32_t patch)  override;
    Return<void> getAudioPort(const AudioPort &port,
                              getAudioPort_cb _hidl_cb)  override;
    Return<Result> setAudioPortConfig(const AudioPortConfig &config)  override;
    Return<AudioHwSync> getHwAvSync()  override;
    Return<Result> setScreenState(bool turnedOn)  override;
    Return<void> getParameters(
        const hidl_vec<hidl_string> &keys, getParameters_cb _hidl_cb)  override;
    Return<Result> setParameters(const hidl_vec<ParameterValue> &parameters)
    override;
    Return<void> debugDump(const hidl_handle &fd)  override;

    // Utility methods for extending interfaces.
    Result analyzeStatus(const char *funcName, int status);
    void closeInputStream(IStreamIn *stream);
    void closeOutputStream(IStreamOut *stream);
    const IDevice *device() const {
        return this;
    }

    int setVoiceVolume(float volume);
    int setMode(audio_mode_t mode);
    int checkInputParameters(uint32_t sampleRateHz, AudioFormat format, int channel_count);
    int setRouteByArray(mixer *_mixer, route_setting *route, int enable);
    Return<void> openOutputStreamLowLatency(int32_t ioHandle,
                                      const DeviceAddress &device,
                                      const AudioConfig &config,
                                      AudioOutputFlag flags,
                                      openOutputStream_cb _hidl_cb);
    Return<void> openOutputStreamHdmi(int32_t ioHandle,
                                      const DeviceAddress &device,
                                      const AudioConfig &config,
                                      AudioOutputFlag flags,
                                      openOutputStream_cb _hidl_cb);

private:

    virtual ~Device();

    // Methods from ParametersUtil.
    char *halGetParameters(const char *keys) override;
    int halSetParameters(const char *keysAndValues) override;

    uint32_t version() const {
        return 2;
    }

public:
    static pthread_mutex_t lock;       /* see note below on mutex acquisition order */
    mixer *_mixer;
    mixer_ctls _mixer_ctls;
    audio_mode_t mode;
    int devices;
    IStreamIn *active_input;
    IStreamOut *outputs[OUTPUT_TOTAL];
    bool mic_mute;
    int tty_mode;
    echo_reference_itfe *echo_reference;
};

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_AUDIO_V2_0_DEVICE_H
