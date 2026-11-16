#
# Copyright (C) 2023 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# Retain eng debugging while enabling normal ART precompilation and dexopt.
# This must precede full_base -> runtime_libart.mk.
OVERRIDE_DISABLE_DEXOPT_ALL := false

# Inherit from those products. Most specific first.
$(call inherit-product, $(SRC_TARGET_DIR)/product/core_64_bit.mk)
$(call inherit-product, $(SRC_TARGET_DIR)/product/full_base.mk)

# Inherit from gts9uwifi device
$(call inherit-product, device/samsung/gts9uwifi/device.mk)

# Keep the version shown in Settings free of the device codename.
LINEAGE_VERSION_SHOW_DEVICE := false

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
WITH_GMS := false
WITH_GMS_COMMS_SUITE := false

PRODUCT_GMS_CLIENTID_BASE := android-samsung-ss

# Generate the fingerprint from this build; show only its ID in Settings.
PRODUCT_BUILD_PROP_OVERRIDES += \
    LineageDesc=$(BUILD_ID) \
    DeviceProduct=gts9uwifixx \
    SystemName=gts9uwifixx

# Compile system-server code ahead of time for predictable startup latency.
PRODUCT_SYSTEM_SERVER_COMPILER_FILTER := speed
