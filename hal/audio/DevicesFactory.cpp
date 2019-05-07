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

#include <string.h>

#include <log/log.h>

#include "Device.h"
#include "DevicesFactory.h"
#include "PrimaryDevice.h"
#include "Util.h"

#include <system/audio.h>

namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

// static
const char *DevicesFactory::deviceToString(IDevicesFactory::Device device) {
    switch (device) {
    case IDevicesFactory::Device::PRIMARY: return AUDIO_HARDWARE_MODULE_ID_PRIMARY;
    case IDevicesFactory::Device::A2DP: return AUDIO_HARDWARE_MODULE_ID_A2DP;
    case IDevicesFactory::Device::USB: return AUDIO_HARDWARE_MODULE_ID_USB;
    case IDevicesFactory::Device::R_SUBMIX: return
            AUDIO_HARDWARE_MODULE_ID_REMOTE_SUBMIX;
    case IDevicesFactory::Device::STUB: return AUDIO_HARDWARE_MODULE_ID_STUB;
    }
    return nullptr;
}

DevicesFactory::DevicesFactory() {

}

DevicesFactory::~DevicesFactory() {

}

// Methods from ::android::hardware::audio::V2_0::IDevicesFactory follow.
Return<void> DevicesFactory::openDevice(IDevicesFactory::Device device,
                                        openDevice_cb _hidl_cb) {
    ALOGI("%s start function", __func__);
    Result retval(Result::INVALID_ARGUMENTS);
    sp<IPrimaryDevice> result = nullptr;
    if (device == IDevicesFactory::Device::PRIMARY) {
        try {
            result = new PrimaryDevice();
            retval = Result::OK;
        } catch (error_types et) {
            ALOGE("%s failed create device", __func__);
            result = NULL;
            retval = Result::NOT_INITIALIZED;
        }
    } else
        retval = Result::INVALID_ARGUMENTS;
    _hidl_cb(retval, result);
    return Void();
}

}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android
