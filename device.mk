#
# Copyright (C) 2024 The LineageOS Project
#
# SPDX-License-Identifier: Apache-2.0
#

# AAPT
PRODUCT_AAPT_CONFIG := normal
PRODUCT_AAPT_PREF_CONFIG := xxxhdpi

# Tablet
TARGET_IS_TABLET := true
TARGET_IS_WIFI-ONLY := true

# Characteristics
PRODUCT_CHARACTERISTICS := tablet

# Boot animation
TARGET_SCREEN_HEIGHT := 1600
TARGET_SCREEN_WIDTH := 2560

# Set boot animation orientation and default display rotation to be landscape.
PRODUCT_DEFAULT_PROPERTY_OVERRIDES += \
    ro.bootanim.set_orientation_logical_0=ORIENTATION_90

# Init
# This tablet has no vm-bootsys or bluetooth firmware partitions.
SM8550_FSTAB := device/samsung/gts9uwifi/init/fstab.qcom
$(call soong_config_set,samsung_sm8550_fstab,gts9uwifi,true)

PRODUCT_PACKAGES += \
    init.gts9uwifi.rc

# Namespaces
PRODUCT_SOONG_NAMESPACES += \
    $(LOCAL_PATH)

# Overlay
DEVICE_PACKAGE_OVERLAYS += \
    $(LOCAL_PATH)/overlay \
    $(LOCAL_PATH)/overlay-lineage \
    $(LOCAL_PATH)/overlay-face-orientation

# Activity orientations are read from the Settings APK when it is parsed.
PRODUCT_ENFORCE_RRO_EXCLUDED_OVERLAYS += $(LOCAL_PATH)/overlay-face-orientation

# Samsung commercial device name
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/floating_feature.xml:$(TARGET_COPY_OUT_SYSTEM)/etc/floating_feature.xml

# Built-in display brightness and outdoor HBM
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/displayconfig/display_id_4630946478520245635.xml:$(TARGET_COPY_OUT_VENDOR)/etc/displayconfig/display_id_4630946478520245635.xml

# Security service transport for this Wi-Fi-only tablet
PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/ssg/sec_channel.rc:$(TARGET_COPY_OUT_VENDOR)/etc/sec_channel.rc

# S Pen firmware lookup during I2C probe.
PRODUCT_PACKAGES += spen_firmware_early_symlink

# Signed cfg80211 regulatory database from the AOSP wireless-regdb project.
# Its signer is included in this kernel's built-in regulatory certificates.
PRODUCT_COPY_FILES += \
    external/wireless-regdb/regulatory.db:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db \
    external/wireless-regdb/regulatory.db.p7s:$(TARGET_COPY_OUT_VENDOR)/firmware/regulatory.db.p7s

# WiFi firmware symlinks
PRODUCT_PACKAGES += \
    firmware_wlanmdsp.otaupdate_symlink \
    firmware_wlan_mac.bin_symlink \
    firmware_WCNSS_qcom_cfg.ini_symlink \
    firmware_wlan_mac.bin_kiwi_v2_symlink \
    firmware_WCNSS_qcom_cfg.ini_kiwi_v2_symlink

# Samsung IDC files
PRODUCT_COPY_FILES += \
    $(call find-copy-subdir-files,*,$(LOCAL_PATH)/configs/idc/,$(TARGET_COPY_OUT_VENDOR)/usr/idc)

# ANGLE
PRODUCT_PACKAGES += \
    ANGLE

# SamsungParts
PRODUCT_PACKAGES += \
   SPenActions \
   vendor.samsung.hardware.spen-service

# Tablet core
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/tablet_core_hardware.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/tablet_core_hardware.xml

PRODUCT_COPY_FILES += \
    $(LOCAL_PATH)/configs/powerhint.json:$(TARGET_COPY_OUT_VENDOR)/etc/powerhint.json

# Samsung's CYG1 face HAL uses the framework FaceService via IFace/default.
PRODUCT_COPY_FILES += \
    frameworks/native/data/etc/android.hardware.biometrics.face.xml:$(TARGET_COPY_OUT_VENDOR)/etc/permissions/android.hardware.biometrics.face.xml

# FOD
$(call soong_config_set,samsungUdfpsVars,udfps_zorder,0x20000000u)
$(call soong_config_set,samsungUdfpsVars,dim_layer_zorder,0x20000001u)
$(call soong_config_set,qtidisplay,samsung_udfps,true)
$(call soong_config_set, qtidisplay, udfps, true)
$(call soong_config_set,surfaceflinger,udfps_lib,//hardware/samsung:libudfps_extension.samsung)
$(call soong_config_set,samsungTouchVars,enableGloveMode,false)
$(call soong_config_set,samsungTouchVars,enableHighTouchPollingRate,false)
$(call soong_config_set,samsungTouchVars,enableStylusMode,false)
# Process the tablet speaker's PCM output in the vendor audio HAL.
$(call soong_config_set_bool,android_hardware_audio,gts9u_speaker_dolby,true)
SM8550_AUDIO_SPEAKER_ROUTE := device/samsung/gts9uwifi/audio/audio_policy_speaker_route.xml

# Inherit from the common OEM chipset makefile.
SM8550_AUDIO_EFFECTS_XML := device/samsung/gts9uwifi/audio/audio_effects.xml
SM8550_AUDIO_POLICY_CONFIGURATION := device/samsung/gts9uwifi/audio/audio_policy_configuration.xml

$(call inherit-product, device/samsung/sm8550-common/common.mk)

# Inherit from the proprietary files makefile.
$(call inherit-product, vendor/samsung/gts9uwifi/gts9uwifi-vendor.mk)

# Samsung Dolby speaker control and its audio app overrides.
PRODUCT_PACKAGES += \
    SamsungDAP \
    Gts9uAudioPackageOverrides

# Samsung Wi-Fi MAC provisioning before CNSS cold calibration.
PRODUCT_PACKAGES += \
    macloader \
    init.gts9uwifi.wifi.rc

# Legacy CYG1 clients still inspect the text effects configuration.
PRODUCT_COPY_FILES += \
    device/samsung/gts9uwifi/audio/audio_effects.conf:$(TARGET_COPY_OUT_VENDOR)/etc/audio/sku_kalama/audio_effects.conf
