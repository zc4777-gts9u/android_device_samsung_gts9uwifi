#
# Copyright (C) 2023 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit_only.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)

# Inherit from gts9uwifi device
$(call inherit-product, device/samsung/gts9uwifi/device.mk)

# Inherit some common Lineage stuff.
$(call inherit-product, vendor/lineage/config/common_full_tablet_wifionly.mk)

PRODUCT_DEVICE := gts9uwifi
PRODUCT_NAME := lineage_gts9uwifi
PRODUCT_BRAND := samsung
PRODUCT_MODEL := SM-X910
PRODUCT_MANUFACTURER := samsung

## S Pen
TARGET_HAVE_SPEN := true

# GMS
WITH_GMS := true
WITH_GMS_COMMS_SUITE := false

PRODUCT_GMS_CLIENTID_BASE := android-samsung-ss

PRODUCT_BUILD_PROP_OVERRIDES += \
    BuildDesc=$(call normalize-path-list,"gts9uwifixx-user 15 AP3A.240905.015.A2 X910XXS5CYG1 release-keys") \
    BuildFingerprint=samsung/gts9uwifixx/gts9uwifi:15/AP3A.240905.015.A2/X910XXS5CYG1:user/release-keys \
    DeviceProduct=gts9uwifixx \
    SystemName=gts9uwifixx
