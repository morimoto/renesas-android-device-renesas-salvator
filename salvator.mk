# Copyright (C) 2017 GlobalLogic
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

TARGET_BOARD_PLATFORM ?= r8a7795
RELEASE_NUMBER ?= test

$(call inherit-product, device/renesas/salvator/device.mk)

PRODUCT_NAME := salvator
PRODUCT_DEVICE := salvator
PRODUCT_BRAND := Renesas
PRODUCT_MODEL := Salvator-X-$(TARGET_BOARD_PLATFORM)
PRODUCT_MANUFACTURER := Renesas

BUILD_VERSION_TAGS += $(TARGET_BOARD_PLATFORM) $(RELEASE_NUMBER)
