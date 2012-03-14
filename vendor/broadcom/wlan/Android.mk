#
# Copyright (C) 2008 Broadcom Corporation
#
# $Id: Android.mk,v 2.6 2009-05-07 18:25:15 Exp $
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
ifdef BOARD_WLAN_BROADCOM

LOCAL_PATH:= $(call my-dir)

ifneq ($(TARGET_BOARD_PLATFORM),)

#Build BCM4330 Driver
include $(call all-subdir-makefiles)
include $(CLEAR_VARS)

SRC_MAIN := dhd/linux
SAVED_LOCAL_PATH := $(LOCAL_PATH)
LOCAL_PATH_ABS := $(shell cd $(SAVED_LOCAL_PATH) && pwd)

BUILD_ROOT := $(ANDROID_PRODUCT_OUT)/obj/BCM4330_DRV

BUILD_SUBDIRS := $(shell cd $(SAVED_LOCAL_PATH) && find . -type d)
BUILD_SRC := $(shell cd $(SAVED_LOCAL_PATH) && find . -name \*[ch])
BUILD_MK := $(shell cd $(SAVED_LOCAL_PATH) && find . -name \*[Mm]akefile\*)
BUILD_MK += $(shell cd $(SAVED_LOCAL_PATH) && find . -name [Mm]akerules\*)

REL_SUBDIRS := $(addprefix $(SAVED_LOCAL_PATH)/, $(BUILD_SUBDIRS))
REL_SRC := $(addprefix $(SAVED_LOCAL_PATH)/, $(BUILD_SRC))
REL_MK := $(addprefix $(SAVED_LOCAL_PATH)/, $(BUILD_MK))

$(BUILD_ROOT)/$(SRC_MAIN)/dhd-android/bcm4330.ko: $(PRODUCT_OUT)/kernel prep
	@(cd $(BUILD_ROOT)/$(SRC_MAIN); $(MAKE) -f Makefile)

prep: subdirs src mk

subdirs: $(REL_SUBDIRS)
	@(for i in $(BUILD_SUBDIRS); do mkdir -p $(BUILD_ROOT)/$$i; done)

src: $(REL_SRC) subdirs
	@(for i in $(BUILD_SRC); do test -e $(BUILD_ROOT)/$$i || ln -sf $(LOCAL_PATH_ABS)/$$i $(BUILD_ROOT)/$$i; done)

mk: $(REL_MK) subdirs
	@(for i in $(BUILD_MK); do test -e $(BUILD_ROOT)/$$i || ln -sf $(LOCAL_PATH_ABS)/$$i $(BUILD_ROOT)/$$i; done)

# copy the modules
$(TARGET_OUT)/lib/modules/bcm4330.ko: $(BUILD_ROOT)/$(SRC_MAIN)/dhd-android/bcm4330.ko | $(ACP)
	$(transform-prebuilt-to-target)

ALL_PREBUILT += $(TARGET_OUT)/lib/modules/bcm4330.ko

endif # ifneq ($(TARGET_BOARD_PLATFORM),)

endif
