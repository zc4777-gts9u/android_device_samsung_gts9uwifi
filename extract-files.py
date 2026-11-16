#!/usr/bin/env -S PYTHONPATH=../../../tools/extract-utils python3
#
# SPDX-FileCopyrightText: 2024 The LineageOS Project
# SPDX-License-Identifier: Apache-2.0
#

from hashlib import sha1
from pathlib import Path
from struct import pack_into, unpack_from

from extract_utils.fixups_blob import (
    BlobFixupCtx,
    blob_fixup,
    blob_fixups_user_type,
)
from extract_utils.fixups_lib import (
    lib_fixups,
    lib_fixups_user_type,
)
from extract_utils.file import File
from extract_utils.main import (
    ExtractUtils,
    ExtractUtilsModule,
)

namespace_imports = [
    'device/samsung/gts9uwifi',
    'device/samsung/sm8550-common',
    'hardware/qcom-caf/sm8550',
    'vendor/qcom/opensource/commonsys-intf/display',
    'vendor/samsung/sm8550-common',
]

def lib_fixup_vendor_suffix(lib: str, partition: str, *args, **kwargs):
    return f'{lib}_{partition}' if partition == 'vendor' else None


lib_fixups: lib_fixups_user_type = {
    **lib_fixups,
    (
        'libsecril-client',
        'vendor.qti.hardware.fm@1.0',
    ): lib_fixup_vendor_suffix,
}

def fix_face_preview(
    _ctx: BlobFixupCtx, _file: File, file_path: str, **_kwargs,
):
    path = Path(file_path)
    contents = bytearray(path.read_bytes())
    if sha1(contents).hexdigest() != '0254f446de6531c5cae33583052aa73f0c8c5246':
        raise ValueError('Face preview conversion patch requires the CYG1 face service')

    # getANativeWindow inlines a VNDK 33 buffer producer constructor. Replace it
    # with a branch to the Surface constructor's PLT entry, used only here.
    entry = 0x12840
    plt = 0x188b0
    branch = 0x14000000 | ((plt - entry) >> 2)
    contents[entry:entry + 4] = branch.to_bytes(4, 'little')

    # Redirect the unused constructor import to the matching current-ABI wrapper.
    constructor = b'_ZN7android7SurfaceC1ERKNS_2spINS_22IGraphicBufferProducerEEEbRKNS1_INS_7IBinderEEE\0'
    replacement = b'gts9u_face_preview_window'
    if contents.count(constructor) != 1:
        raise ValueError('Expected a single Surface constructor import in the CYG1 face service')
    contents = contents.replace(constructor, replacement.ljust(len(constructor), b'\0'))
    path.write_bytes(contents)


def fix_face_camera_window(
    _ctx: BlobFixupCtx, _file: File, file_path: str, **_kwargs,
):
    path = Path(file_path)
    contents = path.read_bytes()
    if sha1(contents).hexdigest() != 'd93cb6bc583341d281c02cdc077cc3f2d2a14280':
        raise ValueError('Face camera window patch requires the CYG1 FaceService library')

    # Current vendor NDK camera outputs take ANativeWindow*, not the old
    # HIDL native_handle_t*. SurfaceManager only stores this borrowed pointer;
    # AImageReader owns its lifetime in both APIs. Preserve LIBMEDIANDK version.
    old = b'AImageReader_getWindowNativeHandle\0'
    new = b'AImageReader_getWindow\0'
    if contents.count(old) != 1:
        raise ValueError('Expected one CYG1 image reader window import')
    contents = bytearray(contents.replace(old, new.ljust(len(old), b'\0')))
    # CYG1 uses secure camera 4 and sends its buffers to the face TA. AOSP's
    # camera path leaves this stream unprotected in the captured logs. Request
    # protected buffers instead of CPU_READ_OFTEN, so gralloc allocates from
    # the camera-protected display heap rather than the scattered system heap.
    # AImageClient::init: mov w8, #3 -> mov w8, #0x4000.
    offset = 0x5f3fc
    if contents[offset:offset + 4] != bytes.fromhex('68008052'):
        raise ValueError('Unexpected CYG1 face image reader usage instruction')
    contents[offset:offset + 4] = bytes.fromhex('08008852')

    # renderFrame calls fillBuffer through its PLT. Redirect that one function
    # to a current-ABI shim that respects the lockYCbCr planes and CYG1 UV order.
    # The replacement is an import; the original code is no longer referenced.
    old_symbol = b'_ZN7android18FaceDisplayManager10fillBufferER13android_ycbcrRNS_7BufInfoE'
    new_symbol = b'gts9u_face_preview_fill'
    if contents[:6] != b'\x7fELF\x02\x01':
        raise ValueError('Expected little-endian ELF64 FaceService')
    section_offset = unpack_from('<Q', contents, 0x28)[0]
    section_size, section_count = unpack_from('<HH', contents, 0x3a)
    sections = [unpack_from('<IIQQQQIIQQ', contents, section_offset + i * section_size)
                for i in range(section_count)]
    patched = 0
    for section in sections:
        if section[1] != 11:  # SHT_DYNSYM
            continue
        strings = sections[section[6]]
        for entry in range(section[4], section[4] + section[5], section[9]):
            name, info, other, index, value, size = unpack_from('<IBBHQQ', contents, entry)
            position = strings[4] + name
            end = contents.index(0, position)
            if contents[position:end] != old_symbol:
                continue
            if value != 0x528e4 or size != 416 or index == 0 or info != 0x12:
                raise ValueError('Unexpected CYG1 fillBuffer symbol')
            contents[position:end + 1] = new_symbol.ljust(end + 1 - position, b'\0')
            pack_into('<IBBHQQ', contents, entry, name, info, other, 0, 0, 0)
            patched += 1
    if patched != 1:
        raise ValueError('Expected one CYG1 preview fillBuffer symbol')
    path.write_bytes(contents)


blob_fixups: blob_fixups_user_type = {
    'vendor/bin/hw/vendor.samsung.hardware.biometrics.face-service': blob_fixup()
        .call(fix_face_preview, need_tmp_dir=False)
        .add_needed('libgts9u_face_preview.so'),
    # CYG1 FaceService uses the older AHandler layout from VNDK 33.
    'vendor/lib64/libFaceService.so': blob_fixup()
        .call(fix_face_camera_window, need_tmp_dir=False)
        .replace_needed('libstagefright_foundation.so', 'libstagefright_foundation_vendor.so')
        .add_needed('libgts9u_face_preview.so'),
    'vendor/lib64/libsfp_teegw.so': blob_fixup()
        .add_needed('libion_shim.so'),
}

module = ExtractUtilsModule(
    'gts9uwifi',
    'samsung',
    blob_fixups=blob_fixups,
    lib_fixups=lib_fixups,
    namespace_imports=namespace_imports,
)

if __name__ == '__main__':
    utils = ExtractUtils.device_with_common(module, 'sm8550-common', module.vendor)
    utils.run()