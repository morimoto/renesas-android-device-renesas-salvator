/*
 * Copyright (C) 2018 GlobalLogic
 */

#define LOG_TAG "VehicleHalImpl"

#include <utils/SystemClock.h>
#include <log/log.h>
#include <android-base/macros.h>

#include "VehicleHalImpl.h"
#include "DefaultConfig.h"

namespace android {
namespace hardware {
namespace automotive {
namespace vehicle {
namespace V2_0 {
namespace salvator {

#define SIZEOF_BIT_ARRAY(bits)  ((bits + 7) / 8)
#define TEST_BIT(bit, array)    (array[bit / 8] & (1 << (bit % 8)))

VehicleHalImpl::VehicleHalImpl(VehiclePropertyStore* propStore) :
    mPropStore(propStore),
    mHvacPowerProps(std::begin(kHvacPowerProperties), std::end(kHvacPowerProperties)),
    mRecurrentTimer(std::bind(&VehicleHalImpl::onContinuousPropertyTimer,
                                  this, std::placeholders::_1)),
    mGpioThread { &VehicleHalImpl::GpioHandleThread, this },
    mGpioThreadExit(false)
{
    for (size_t i = 0; i < arraysize(kVehicleProperties); i++) {
        mPropStore->registerProperty(kVehicleProperties[i].config);
    }
}

VehicleHalImpl::~VehicleHalImpl(void)
{
    ALOGD("%s: ->", __func__);

    mGpioThreadExit = true; // Notify thread to finish and wait for it to terminate.

    if (mGpioThread.joinable()) mGpioThread.join();

    ALOGD("%s: <-", __func__);
}
 
void VehicleHalImpl::onCreate(void)
{
    for (auto& it : kVehicleProperties)
    {
        VehiclePropConfig cfg = it.config;
        int32_t numAreas = cfg.areaConfigs.size();

//        if (isDiagnosticProperty(cfg)) {
            // do not write an initial empty value for the diagnostic properties
            // as we will initialize those separately.
//            continue;
//        }

        //  A global property will have supportedAreas = 0
        if (isGlobalProp(cfg.prop)) {
            numAreas = 1;
        }

        // This loop is a do-while so it executes at least once to handle global properties
        for (int i = 0; i < numAreas; i++)
        {
            int32_t curArea;

            if (isGlobalProp(cfg.prop)) {
                curArea = 0;
            } else {
                curArea = cfg.areaConfigs[i].areaId;
            }

            // Create a separate instance for each individual zone
            VehiclePropValue prop = {
                .prop = cfg.prop,
                .areaId = curArea,
            };
            if (it.initialAreaValues.size() > 0) {
                auto valueForAreaIt = it.initialAreaValues.find(curArea);
                if (valueForAreaIt != it.initialAreaValues.end()) {
                    prop.value = valueForAreaIt->second;
                } else {
                    ALOGW("%s failed to get default value for prop 0x%x area 0x%x",
                            __func__, cfg.prop, curArea);
                }
            } else {
                prop.value = it.initialValue;
            }
            mPropStore->writeValue(prop, true);

        }
    }
}

std::vector<VehiclePropConfig> VehicleHalImpl::listProperties(void)
{
    return mPropStore->getAllConfigs();
}

VehicleHal::VehiclePropValuePtr VehicleHalImpl::get(const VehiclePropValue& requestedPropValue,
                            StatusCode* outStatus)
{
    VehiclePropValuePtr v = nullptr;

    auto internalPropValue = mPropStore->readValueOrNull(requestedPropValue);
    if (internalPropValue != nullptr) {
        v = getValuePool()->obtain(*internalPropValue);
    }

    ALOGI("..get 0x%08x", requestedPropValue.prop);

    *outStatus = (v != nullptr) ? StatusCode::OK : StatusCode::INVALID_ARG;
    return v;
}

StatusCode VehicleHalImpl::set(const VehiclePropValue& propValue)
{
     if (mHvacPowerProps.count(propValue.prop)) {
        auto hvacPowerOn = mPropStore->readValueOrNull(toInt(VehicleProperty::HVAC_POWER_ON),
                                                      toInt(VehicleAreaSeat::ROW_1_CENTER));

        if (hvacPowerOn && hvacPowerOn->value.int32Values.size() == 1
                && hvacPowerOn->value.int32Values[0] == 0) {
            return StatusCode::NOT_AVAILABLE;
        }
    }

    if (!mPropStore->writeValue(propValue, true)) {
        return StatusCode::INVALID_ARG;
    }

    ALOGD("..set 0x%08x areaId=0x%x int32Values=%zu floatValues=%zu int64Values=%zu bytes=%zu string='%s'", propValue.prop, propValue.areaId,
        propValue.value.int32Values.size(),
        propValue.value.floatValues.size(),
        propValue.value.int64Values.size(),
        propValue.value.bytes.size(),
        propValue.value.stringValue.c_str());

        for(size_t i = 0; i < propValue.value.int32Values.size(); i++)
            ALOGD("int32Values[%zu]=%d", i, propValue.value.int32Values[i]);

        for(size_t i = 0; i < propValue.value.floatValues.size(); i++)
            ALOGD("floatValues[%zu]=%f", i, propValue.value.floatValues[i]);

        for(size_t i = 0; i < propValue.value.int64Values.size(); i++)
            ALOGD("int64Values[%zu]=%" PRId64 " ", i, propValue.value.int64Values[i]);

    return StatusCode::OK;
}

StatusCode VehicleHalImpl::subscribe(int32_t property, float sampleRate)
{
    ALOGI("%s propId: 0x%x, sampleRate: %f", __func__, property, sampleRate);

    if (isContinuousProperty(property)) {
        mRecurrentTimer.registerRecurrentEvent(hertzToNanoseconds(sampleRate), property);
    }
    return StatusCode::OK;
}

StatusCode VehicleHalImpl::unsubscribe(int32_t property)
{
    ALOGI("%s propId: 0x%x", __func__, property);
    if (isContinuousProperty(property)) {
        mRecurrentTimer.unregisterRecurrentEvent(property);
    }
    return StatusCode::OK;
}

void VehicleHalImpl::onContinuousPropertyTimer(const std::vector<int32_t>& properties)
{
    VehiclePropValuePtr v;

    auto& pool = *getValuePool();

    for (int32_t property : properties) {
        if (isContinuousProperty(property)) {
            auto internalPropValue = mPropStore->readValueOrNull(property);
            if (internalPropValue != nullptr) {
                v = pool.obtain(*internalPropValue);
            }
        } else {
            ALOGE("Unexpected onContinuousPropertyTimer for property: 0x%x", property);
        }

        if (v.get()) {
            v->timestamp = elapsedRealtimeNano();
            doHalEvent(std::move(v));
        }
    }
}

void VehicleHalImpl::onGpioStateChanged(int fd, unsigned char* const key_bitmask, size_t array_len)
{
    VehiclePropValue propValue = {
        .prop = toInt(VehicleProperty::GEAR_SELECTION),
        .areaId = toInt(VehicleArea::GLOBAL),
        .timestamp = elapsedRealtimeNano(),
        .value.int32Values = {toInt(VehicleGear::GEAR_NEUTRAL)}
    };

    if (ioctl(fd, EVIOCGKEY(array_len), key_bitmask) >= 0) {

        if (TEST_BIT(reverse_switch, key_bitmask)) { /* SW2 - 4 */
            propValue.value.int32Values[0] = toInt(VehicleGear::GEAR_REVERSE);
            ALOGI("Curreant gear: REVERSE");
        } else if (TEST_BIT(parking_switch, key_bitmask)) { /* SW2 - 3 */
            propValue.value.int32Values[0] = toInt(VehicleGear::GEAR_PARK);
            ALOGI("Curreant gear: PARKING");
        } else {
            ALOGI("Curreant gear: NEUTRAL");
        }

        if (mPropStore->writeValue(propValue, true)) {
            if (getValuePool() != NULL) {
                doHalEvent(getValuePool()->obtain(propValue));
            } else {
                ALOGW("getValuePool() == NULL: propId: 0x%x", propValue.prop);
            }
        }
    }
}

bool VehicleHalImpl::isContinuousProperty(int32_t propId) const
{
    const VehiclePropConfig* config = mPropStore->getConfigOrNull(propId);
    if (config == nullptr) {
        ALOGW("Config not found for property: 0x%x", propId);
        return false;
    }
    return config->changeMode == VehiclePropertyChangeMode::CONTINUOUS;
}

void VehicleHalImpl::GpioHandleThread(void)
{
    if (mGpioThreadExit) return;

    ALOGD("GpioHandleThread() ->");

    int fd = open("/dev/input/event0", O_RDONLY);

    if (fd < 0) {
        ALOGE("Could not open input event device, error: %s\n", strerror(errno));
        return;
    };

    unsigned char key_bitmask[SIZEOF_BIT_ARRAY(KEY_MAX + 1)];
    memset(key_bitmask, 0, sizeof(key_bitmask));

    struct pollfd fds = {
        .fd = fd, .events = POLLIN, .revents = POLLOUT
    };

    /**
     * Here is we check initial GPIO switches state
     * in case we want to boot up straight into
     * the EVS app.
     */
    onGpioStateChanged(fd, key_bitmask, sizeof(key_bitmask));

    while (!mGpioThreadExit) {

        if (poll(&fds, 1, -1) > 0) {

            if (read(fds.fd, key_bitmask, sizeof(key_bitmask)) > 0) {
                onGpioStateChanged(fd, key_bitmask, sizeof(key_bitmask));
            }
        }
    }

    close(fd);

    ALOGD("GpioHandleThread() <-");
}

}  // namespace salvator
}  // namespace V2_0
}  // namespace vehicle
}  // namespace automotive
}  // namespace hardware
}  // namespace android
