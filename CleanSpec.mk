# SPDX-License-Identifier: Apache-2.0

ifeq ($(TARGET_DEVICE),gts9uwifi)
# The default HIDL library is searched before the Samsung implementation.
$(call add-clean-step, rm -f $(TARGET_OUT_VENDOR)/lib/hw/android.hardware.audio@7.1-impl.so $(TARGET_OUT_VENDOR)/lib64/hw/android.hardware.audio@7.1-impl.so $(PRODUCT_OUT)/vendor.img $(PRODUCT_OUT)/obj/PACKAGING/vendor_intermediates/vendor.img)
endif
