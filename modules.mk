#
# Copyright (C) 2011 The Android Open-Source Project
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

PRODUCT_OUT               := $(OUT_DIR)/target/product/$(TARGET_PRODUCT)
PRODUCT_OUT_ABS           := $(abspath $(PRODUCT_OUT))

TARGET_KERNEL_MODULES_OUT := $(PRODUCT_OUT_ABS)/obj/KERNEL_MODULES


BOARD_VENDOR_KERNEL_MODULES += \
	$(TARGET_KERNEL_MODULES_OUT)/usbserial.ko \
	$(TARGET_KERNEL_MODULES_OUT)/pl2303.ko \
	$(TARGET_KERNEL_MODULES_OUT)/ftdi_sio.ko \
	$(TARGET_KERNEL_MODULES_OUT)/cdc-acm.ko \
	$(TARGET_KERNEL_MODULES_OUT)/uvcvideo.ko

BOARD_VENDOR_KERNEL_MODULES += \
	$(TARGET_KERNEL_MODULES_OUT)/rtlwifi.ko \
	$(TARGET_KERNEL_MODULES_OUT)/rtl_usb.ko \
	$(TARGET_KERNEL_MODULES_OUT)/rtl8192c-common.ko \
	$(TARGET_KERNEL_MODULES_OUT)/rtl8192cu.ko

BOARD_VENDOR_KERNEL_MODULES += \
	$(TARGET_KERNEL_MODULES_OUT)/btbcm.ko \
	$(TARGET_KERNEL_MODULES_OUT)/btintel.ko \
	$(TARGET_KERNEL_MODULES_OUT)/btrtl.ko \
	$(TARGET_KERNEL_MODULES_OUT)/btusb.ko

include device/renesas/common/ModulesCommon.mk
