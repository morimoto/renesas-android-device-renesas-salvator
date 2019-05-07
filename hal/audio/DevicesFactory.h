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

#ifndef ANDROID_HARDWARE_AUDIO_V2_0_DEVICESFACTORY_H_
#define ANDROID_HARDWARE_AUDIO_V2_0_DEVICESFACTORY_H_


#include <android/hardware/audio/2.0/IDevicesFactory.h>
#include <hidl/Status.h>

#include <hidl/MQDescriptor.h>
namespace android {
namespace hardware {
namespace audio {
namespace V2_0 {
namespace salvator {

using ::android::hardware::audio::V2_0::IDevice;
using ::android::hardware::audio::V2_0::IDevicesFactory;
using ::android::hardware::audio::V2_0::Result;
using ::android::hardware::Return;
using ::android::hardware::Void;
using ::android::hardware::hidl_vec;
using ::android::hardware::hidl_string;
using ::android::sp;

struct DevicesFactory : public IDevicesFactory {

    // Methods from ::android::hardware::audio::V2_0::IDevicesFactory follow.
    Return<void> openDevice(IDevicesFactory::Device device,
                            openDevice_cb _hidl_cb)  override;
    DevicesFactory();
    ~DevicesFactory() override;

private:
    static const char *deviceToString(IDevicesFactory::Device device);
};


}  // namespace salvator
}  // namespace V2_0
}  // namespace audio
}  // namespace hardware
}  // namespace android

#endif  // ANDROID_HARDWARE_AUDIO_V2_0_DEVICESFACTORY_H
