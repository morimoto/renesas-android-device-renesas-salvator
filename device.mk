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

# Audio
PRODUCT_PACKAGES += \
    android.hardware.audio@2.0-service.salvator \
    android.hardware.audio.effect@4.0-service.renesas

PRODUCT_COPY_FILES += \
    device/renesas/salvator/hal/audio/policy/audio_policy_configuration.xml:$(TARGET_COPY_OUT_ODM)/etc/audio_policy_configuration.xml \
    device/renesas/salvator/hal/audio/policy/audio_policy_volumes.xml:$(TARGET_COPY_OUT_ODM)/etc/audio_policy_volumes.xml \
    device/renesas/salvator/hal/audio/policy/default_volume_tables.xml:$(TARGET_COPY_OUT_ODM)/etc/default_volume_tables.xml

# Bluetooth
PRODUCT_PACKAGES += \
    android.hardware.bluetooth@1.0-service.salvator

# Wi-Fi
PRODUCT_PACKAGES += \
    android.hardware.wifi@1.0-service

# Touchcreen configuration
PRODUCT_COPY_FILES += \
    device/renesas/salvator/touchscreen_skeleton.idc:$(TARGET_COPY_OUT_ODM)/usr/idc/touchscreen_skeleton.idc
