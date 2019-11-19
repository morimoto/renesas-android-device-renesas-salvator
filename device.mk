#
# Copyright (C) 2018 GlobalLogic
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#

$(call inherit-product, device/renesas/common/DeviceCommon.mk)
$(call inherit-product, device/renesas/salvator/modules.mk)

# ----------------------------------------------------------------------
PRODUCT_COPY_FILES += \
    device/renesas/salvator/permissions/privapp-permissions-salvator.xml:$(TARGET_COPY_OUT_ODM)/etc/permissions/privapp-permissions-salvator.xml

# Init RC files
PRODUCT_COPY_FILES += \
    device/renesas/salvator/init/init.salvator.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.salvator.rc \
    device/renesas/salvator/init/init.salvator.usb.rc:$(TARGET_COPY_OUT_VENDOR)/etc/init/hw/init.salvator.usb.rc \
    device/renesas/salvator/init/ueventd.salvator.rc:$(TARGET_COPY_OUT_VENDOR)/ueventd.rc \
    device/renesas/salvator/init/init.recovery.salvator.rc:root/init.recovery.salvator.rc

# Bluetooth HAL (system/bt/vendor_libs/linux/interface)
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-service.salvator

# Touchcreen configuration
PRODUCT_COPY_FILES += \
    device/renesas/salvator/touchscreen_skeleton.idc:$(TARGET_COPY_OUT_ODM)/usr/idc/touchscreen_skeleton.idc

PRODUCT_PACKAGES += \
    libwpa_client \
    libwifilogd \
    wpa_supplicant.conf \
    p2p_supplicant.conf \
    libwifi-hal \
    android.hardware.wifi.supplicant@1.0-service \
    android.hardware.wifi.supplicant@1.1-service \
    android.hardware.wifi.supplicant@1.2-service \
    android.hardware.wifi@1.0-service \
    android.hardware.wifi@1.0-service-lib \

PRODUCT_PROPERTY_OVERRIDES += \
    wifi.direct.interface=p2p0 \
    wifi.interface=wlan0
