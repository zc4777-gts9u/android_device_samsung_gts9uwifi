// SPDX-License-Identifier: Apache-2.0

#define LOG_TAG "Gts9uFacePreview"

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <system/graphics.h>

#include <cutils/native_handle.h>
#include <gui/Surface.h>
#include <gui/bufferqueue/1.0/H2BGraphicBufferProducer.h>
#include <hidl/HybridInterface.h>
#include <log/log.h>

namespace {
using android::HalToken;
using android::sp;
using android::Surface;
using android::hardware::graphics::bufferqueue::V1_0::IGraphicBufferProducer;
using android::hardware::graphics::bufferqueue::V1_0::utils::H2BGraphicBufferProducer;

constexpr int kMaxTokenSize = 256;
}  // namespace

extern "C" ANativeWindow* gts9u_face_preview_window(const void*, const native_handle_t* handle) {
    if (handle == nullptr) {
        return nullptr;
    }
    if (handle->numFds != 0 || handle->numInts <= 1) {
        ALOGE("Unexpected face preview handle");
        return nullptr;
    }

    // SurfaceToNativeHandleConverter stores the token size followed by the token bytes.
    const int size = handle->data[0];
    if (size <= 0 || size > kMaxTokenSize ||
        (size + sizeof(int) - 1) / sizeof(int) > static_cast<size_t>(handle->numInts - 1)) {
        ALOGE("Invalid face preview token size");
        return nullptr;
    }

    HalToken token;
    token.resize(size);
    std::memcpy(token.data(), &handle->data[1], size);
    auto result = IGraphicBufferProducer::castFrom(android::retrieveHalInterface(token));
    if (!result.isOk()) {
        ALOGE("Unable to resolve face preview producer");
        return nullptr;
    }

    sp<IGraphicBufferProducer> producer = result;
    if (producer == nullptr) {
        ALOGE("Face preview producer is missing");
        return nullptr;
    }

    // The caller retains the ANativeWindow, as with the original CYG1 conversion.
    return new Surface(sp<H2BGraphicBufferProducer>::make(producer), true);
}

namespace {
// Offsets are from the SHA1-pinned CYG1 FaceDisplayManager and BufInfo layouts.
// Read fields without relying on the vendor C++ class definitions or aliasing.
template <typename T>
T readField(const void* object, size_t offset) {
    T value;
    std::memcpy(&value, static_cast<const uint8_t*>(object) + offset, sizeof(T));
    return value;
}
}  // namespace

extern "C" int gts9u_face_preview_fill(
        const void* manager, android_ycbcr& output, const void* frame) {
    if (manager == nullptr || frame == nullptr) {
        return -2;
    }
    const auto begin = readField<uintptr_t>(manager, 0xa8);
    const auto end = readField<uintptr_t>(manager, 0xb0);
    const auto width = readField<uint32_t>(manager, 0xc4);
    const auto height = readField<uint32_t>(manager, 0xc8);
    const auto stride = readField<uint32_t>(frame, 0x20);
    const auto sliceHeight = readField<uint32_t>(frame, 0x24);
    const auto frameSize = readField<uint32_t>(frame, 0x30);
    if (width == 0 || height == 0 || width > 8192 || height > 8192 ||
        (width & 1) != 0 || (height & 1) != 0 || stride < width ||
        sliceHeight < height || output.y == nullptr || output.cb == nullptr ||
        output.cr == nullptr || output.ystride < width ||
        (output.chroma_step != 1 && output.chroma_step != 2) ||
        output.cstride < (width / 2 - 1) * output.chroma_step + 1 ||
        begin == 0 || end < begin) {
        ALOGE("Invalid face preview buffer layout");
        return -2;
    }
    const uint64_t chromaOffset = uint64_t(stride) * sliceHeight;
    const uint64_t required = chromaOffset + uint64_t(stride) * (height / 2);
    if (required > frameSize || required > end - begin) {
        ALOGE("Face preview buffer is shorter than its plane layout");
        return -2;
    }

    const auto* input = reinterpret_cast<const uint8_t*>(begin);
    auto* y = static_cast<uint8_t*>(output.y);
    auto* cb = static_cast<uint8_t*>(output.cb);
    auto* cr = static_cast<uint8_t*>(output.cr);
    for (uint32_t row = 0; row < height; ++row) {
        std::memcpy(y + row * output.ystride, input + uint64_t(row) * stride, width);
    }
    // CYG1's protected camera supplies interleaved UV. Write the named planes
    // from lockYCbCr instead of copying that row into an assumed NV21 VU plane.
    for (uint32_t row = 0; row < height / 2; ++row) {
        const auto* uv = input + chromaOffset + uint64_t(row) * stride;
        for (uint32_t column = 0; column < width / 2; ++column) {
            const size_t offset = row * output.cstride + column * output.chroma_step;
            cb[offset] = uv[column * 2];
            cr[offset] = uv[column * 2 + 1];
        }
    }
    return 0;
}
