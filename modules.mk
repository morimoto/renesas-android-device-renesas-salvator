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

PRODUCT_OUT         := $(OUT_DIR)/target/product/$(TARGET_PRODUCT)
KERNEL_MODULES_OUT  := $(PRODUCT_OUT)/obj/KERNEL_MODULES

BOARD_VENDOR_KERNEL_MODULES += \
	$(KERNEL_MODULES_OUT)/usbserial.ko \
	$(KERNEL_MODULES_OUT)/pl2303.ko \
	$(KERNEL_MODULES_OUT)/ftdi_sio.ko \
	$(KERNEL_MODULES_OUT)/cdc-acm.ko \
	$(KERNEL_MODULES_OUT)/uvcvideo.ko

BOARD_VENDOR_KERNEL_MODULES += \
	$(KERNEL_MODULES_OUT)/rtlwifi.ko \
	$(KERNEL_MODULES_OUT)/rtl_usb.ko \
	$(KERNEL_MODULES_OUT)/rtl8192c-common.ko \
	$(KERNEL_MODULES_OUT)/rtl8192cu.ko

BOARD_VENDOR_KERNEL_MODULES += \
	$(KERNEL_MODULES_OUT)/btbcm.ko \
	$(KERNEL_MODULES_OUT)/btintel.ko \
	$(KERNEL_MODULES_OUT)/btrtl.ko \
	$(KERNEL_MODULES_OUT)/btusb.ko

# Realtek Wi-Fi driver
BOARD_VENDOR_KERNEL_MODULES += \
	$(KERNEL_MODULES_OUT)/8812au.ko

WLAN_KM_SRC             := hardware/realtek/rtl8812au_km
WLAN_KM_OUT             := $(PRODUCT_OUT)/obj/WLAN_KM_OBJ
WLAN_KM_OUT_ABS         := $(abspath $(WLAN_KM_OUT))
WLAN_KM                 := $(WLAN_KM_OUT)/8812au.ko

$(WLAN_KM):
	mkdir -p $(WLAN_KM_OUT_ABS)
	cp -pR $(WLAN_KM_SRC)/* $(WLAN_KM_OUT_ABS)/
	$(ANDROID_MAKE) -C $(WLAN_KM_OUT_ABS) $(KERNEL_COMPILE_FLAGS) \
		KERNELDIR=$(KERNEL_OUT_ABS) \
		WORKDIR=$(WLAN_KM_OUT_ABS) rcar_defconfig
	$(ANDROID_MAKE) -C $(WLAN_KM_OUT_ABS) $(KERNEL_COMPILE_FLAGS) \
		KERNELDIR=$(KERNEL_OUT_ABS) WORKDIR=$(WLAN_KM_OUT_ABS) \
		M=$(WLAN_KM_OUT_ABS) modules
	cp $(WLAN_KM) $(KERNEL_MODULES_OUT)/

KERNEL_EXT_MODULES += \
	$(WLAN_KM)

include device/renesas/common/ModulesCommon.mk
