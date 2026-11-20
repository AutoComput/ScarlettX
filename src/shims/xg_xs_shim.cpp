#define SCARLETTX_BUILDING_XG_XS 1
#include "scarlettx/gpu/xg_xs_abi.h"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace {

bool TraceEnabled() {
    const char* v = std::getenv("SCARLETTX_TRACE");
    return v && v[0] != '\0' && v[0] != '0';
}

uint64_t AlignUp(uint64_t value, uint64_t align) {
    if (align <= 1) return value;
    return (value + align - 1) & ~(align - 1);
}

uint32_t FormatBytesPerElement(uint32_t dxgi_format) {
    // Common DXGI_FORMAT sizes (bytes per pixel or per 4x4 block for BC formats).
    switch (dxgi_format) {
        case 2: case 3: case 4:             return 16;  // R32G32B32A32_*
        case 6: case 7: case 8:             return 12;  // R32G32B32_*
        case 10: case 11: case 12: case 13:
        case 14: case 16: case 17: case 18: return 8;   // R16G16B16A16_*, R32G32_*
        case 24: case 28: case 29: case 30:
        case 31: case 32: case 40: case 41:
        case 42: case 43: case 45: case 87:
        case 91:                            return 4;   // R8G8B8A8, D32_FLOAT, D24S8, B8G8R8A8
        case 49: case 54: case 55: case 56:
        case 57: case 58: case 59: case 85: return 2;   // R16_*, R8G8_*, B5G6R5
        case 61: case 62: case 63: case 64:
        case 65:                            return 1;   // R8_*, A8_UNORM
        case 70: case 71: case 72:          return 8;   // BC1 (8 bytes / 4x4 block)
        case 73: case 74: case 75:          return 16;  // BC2
        case 76: case 77: case 78:          return 16;  // BC3
        case 79: case 80: case 81:          return 8;   // BC4
        case 82: case 83: case 84:          return 16;  // BC5
        case 94: case 95: case 96:          return 16;  // BC6H
        case 97: case 98: case 99:          return 16;  // BC7
        default:                            return 4;
    }
}

bool IsBlockCompressed(uint32_t dxgi_format) {
    return (dxgi_format >= 70 && dxgi_format <= 84) ||
           (dxgi_format >= 94 && dxgi_format <= 99);
}

void ComputeLayout(const XG_TEXTURE2D_DESC& desc, XG_RESOURCE_LAYOUT& out) {
    std::memset(&out, 0, sizeof(out));
    const uint32_t width = std::max<uint32_t>(1, desc.Width);
    const uint32_t height = std::max<uint32_t>(1, desc.Height);
    const uint32_t array_size = std::max<uint32_t>(1, desc.ArraySize);
    const uint32_t mips = std::clamp<uint32_t>(desc.MipLevels == 0 ? 1u : desc.MipLevels, 1u, 15u);
    const uint32_t bpe = FormatBytesPerElement(desc.Format);
    const bool bc = IsBlockCompressed(desc.Format);

    out.Dimension = 3;  // D3D12_RESOURCE_DIMENSION_TEXTURE2D
    out.MipLevels = mips;
    out.Planes = 1;
    out.BaseAlignmentBytes = 65536;

    XG_PLANE_LAYOUT& plane = out.Plane[0];
    plane.Usage = 0;
    plane.BaseOffsetBytes = 0;
    plane.BaseAlignmentBytes = 65536;
    plane.BytesPerElement = bpe;

    uint64_t offset = 0;
    for (uint32_t m = 0; m < mips; ++m) {
        const uint32_t mw = std::max<uint32_t>(1, width >> m);
        const uint32_t mh = std::max<uint32_t>(1, height >> m);
        const uint32_t we = bc ? std::max<uint32_t>(1, (mw + 3) / 4) : mw;
        const uint32_t he = bc ? std::max<uint32_t>(1, (mh + 3) / 4) : mh;
        const uint32_t pitch_bytes = static_cast<uint32_t>(AlignUp(static_cast<uint64_t>(we) * bpe, 256));
        const uint64_t slice_size = AlignUp(static_cast<uint64_t>(pitch_bytes) * he, 256);
        const uint64_t mip_size = slice_size * array_size;

        XG_MIPLEVEL_LAYOUT& ml = plane.MipLayout[m];
        ml.OffsetBytes = offset;
        ml.SizeBytes = mip_size;
        ml.Slice2DSizeBytes = slice_size;
        ml.PitchBytes = pitch_bytes;
        ml.PitchPixels = pitch_bytes / std::max<uint32_t>(1, bpe);
        ml.AlignmentBytes = 256;
        ml.WidthElements = we;
        ml.HeightElements = he;
        ml.DepthOrArraySize = array_size;
        ml.PaddedWidthElements = we;
        ml.PaddedHeightElements = he;
        ml.PaddedDepthOrArraySize = array_size;
        ml.SampleCount = std::max<uint32_t>(1, desc.SampleDesc.Count);
        ml.TileMode = desc.TileMode;
        ml.SliceDepthElements = 1;

        offset = AlignUp(offset + mip_size, 256);
    }

    plane.SizeBytes = AlignUp(offset, 65536);
    out.SizeBytes = plane.SizeBytes;
}

// Minimal COM vtable matching IXGTexture2DComputer (inherits IUnknown).
struct IXGTexture2DComputerVtbl {
    int32_t (SCARLETTX_XG_CALL* QueryInterface)(void* self, const void* riid, void** ppv);
    uint32_t (SCARLETTX_XG_CALL* AddRef)(void* self);
    uint32_t (SCARLETTX_XG_CALL* Release)(void* self);
    int32_t (SCARLETTX_XG_CALL* GetResourceLayout)(void* self, XG_RESOURCE_LAYOUT* layout);
    uint64_t (SCARLETTX_XG_CALL* GetResourceSizeBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetResourceBaseAlignmentBytes)(void* self);
    uint64_t (SCARLETTX_XG_CALL* GetMipLevelOffsetBytes)(void* self, uint32_t plane, uint32_t miplevel);
    uint64_t (SCARLETTX_XG_CALL* GetTexelElementOffsetBytes)(
        void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t sample);
    int32_t (SCARLETTX_XG_CALL* CopyIntoSubresource)(
        void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t srcDepthPitch);
    int32_t (SCARLETTX_XG_CALL* CopyFromSubresource)(
        void* self, void* dst, uint32_t dstRowPitch, uint32_t dstDepthPitch, const void* src, uint32_t plane, uint32_t subresource);
};

struct XGTexture2DComputerImpl {
    const IXGTexture2DComputerVtbl* lpVtbl;
    std::atomic<uint32_t> ref_count{1};
    XG_TEXTURE2D_DESC desc{};
    XG_RESOURCE_LAYOUT layout{};
};

int32_t SCARLETTX_XG_CALL XgQueryInterface(void* self, const void* /*riid*/, void** ppv) {
    if (!ppv) return static_cast<int32_t>(0x80004003u);  // E_POINTER
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    obj->ref_count.fetch_add(1, std::memory_order_relaxed);
    *ppv = self;
    return 0;  // S_OK
}

uint32_t SCARLETTX_XG_CALL XgAddRef(void* self) {
    return static_cast<XGTexture2DComputerImpl*>(self)->ref_count.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint32_t SCARLETTX_XG_CALL XgRelease(void* self) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    const uint32_t remaining = obj->ref_count.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remaining == 0) {
        delete obj;
    }
    return remaining;
}

int32_t SCARLETTX_XG_CALL XgGetResourceLayout(void* self, XG_RESOURCE_LAYOUT* layout) {
    if (!self || !layout) return static_cast<int32_t>(0x80070057u);
    *layout = static_cast<XGTexture2DComputerImpl*>(self)->layout;
    return 0;
}

uint64_t SCARLETTX_XG_CALL XgGetResourceSizeBytes(void* self) {
    return self ? static_cast<XGTexture2DComputerImpl*>(self)->layout.SizeBytes : 0;
}

uint64_t SCARLETTX_XG_CALL XgGetResourceBaseAlignmentBytes(void* self) {
    return self ? static_cast<XGTexture2DComputerImpl*>(self)->layout.BaseAlignmentBytes : 65536;
}

uint64_t SCARLETTX_XG_CALL XgGetMipLevelOffsetBytes(void* self, uint32_t plane, uint32_t miplevel) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    return obj->layout.Plane[plane].MipLayout[miplevel].OffsetBytes;
}

uint64_t SCARLETTX_XG_CALL XgGetTexelElementOffsetBytes(
    void* self, uint32_t plane, uint32_t miplevel, uint64_t x, uint32_t y, uint32_t zOrSlice, uint32_t /*sample*/) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || plane >= obj->layout.Planes || miplevel >= obj->layout.MipLevels) return 0;
    const auto& pl = obj->layout.Plane[plane];
    const auto& ml = pl.MipLayout[miplevel];
    return ml.OffsetBytes + static_cast<uint64_t>(zOrSlice) * ml.Slice2DSizeBytes +
           static_cast<uint64_t>(y) * ml.PitchBytes + x * pl.BytesPerElement;
}

int32_t SCARLETTX_XG_CALL XgCopyIntoSubresource(
    void* self, void* dst, uint32_t plane, uint32_t subresource, const void* src, uint32_t srcRowPitch, uint32_t /*srcDepthPitch*/) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& ml = obj->layout.Plane[plane].MipLayout[mip];
    auto* dst_base = static_cast<uint8_t*>(dst) + ml.OffsetBytes + static_cast<uint64_t>(slice) * ml.Slice2DSizeBytes;
    const auto* src_base = static_cast<const uint8_t*>(src);
    const uint32_t copy_bytes = std::min(srcRowPitch, ml.PitchBytes);
    for (uint32_t row = 0; row < ml.HeightElements; ++row) {
        std::memcpy(dst_base + static_cast<uint64_t>(row) * ml.PitchBytes,
                    src_base + static_cast<uint64_t>(row) * srcRowPitch,
                    copy_bytes);
    }
    return 0;
}

int32_t SCARLETTX_XG_CALL XgCopyFromSubresource(
    void* self, void* dst, uint32_t dstRowPitch, uint32_t /*dstDepthPitch*/, const void* src, uint32_t plane, uint32_t subresource) {
    auto* obj = static_cast<XGTexture2DComputerImpl*>(self);
    if (!obj || !dst || !src || plane >= obj->layout.Planes) return static_cast<int32_t>(0x80070057u);
    const uint32_t mips = std::max<uint32_t>(1, obj->layout.MipLevels);
    const uint32_t mip = subresource % mips;
    const uint32_t slice = subresource / mips;
    const auto& ml = obj->layout.Plane[plane].MipLayout[mip];
    const auto* src_base = static_cast<const uint8_t*>(src) + ml.OffsetBytes + static_cast<uint64_t>(slice) * ml.Slice2DSizeBytes;
    auto* dst_base = static_cast<uint8_t*>(dst);
    const uint32_t copy_bytes = std::min(dstRowPitch, ml.PitchBytes);
    for (uint32_t row = 0; row < ml.HeightElements; ++row) {
        std::memcpy(dst_base + static_cast<uint64_t>(row) * dstRowPitch,
                    src_base + static_cast<uint64_t>(row) * ml.PitchBytes,
                    copy_bytes);
    }
    return 0;
}

const IXGTexture2DComputerVtbl kVtbl = {
    &XgQueryInterface,
    &XgAddRef,
    &XgRelease,
    &XgGetResourceLayout,
    &XgGetResourceSizeBytes,
    &XgGetResourceBaseAlignmentBytes,
    &XgGetMipLevelOffsetBytes,
    &XgGetTexelElementOffsetBytes,
    &XgCopyIntoSubresource,
    &XgCopyFromSubresource,
};

}  // namespace

extern "C" {

uint32_t SCARLETTX_XG_CALL XGComputeOptimalSwizzleMode(
    uint32_t resourceDimension,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depthOrArraySize,
    uint32_t sampleCount,
    uint32_t bindFlags) {
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][xg_xs] XGComputeOptimalSwizzleMode(dim=" << resourceDimension
                  << ", fmt=" << format << ", " << width << "x" << height
                  << "x" << depthOrArraySize << ", samples=" << sampleCount
                  << ", bind=0x" << std::hex << bindFlags << std::dec << ")\n";
    }
    return XG_SWIZZLE_MODE_64KB_S;
}

int32_t SCARLETTX_XG_CALL XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* desc,
    void** computer) {
    if (!desc || !computer) {
        return static_cast<int32_t>(0x80070057u);  // E_INVALIDARG
    }
    if (TraceEnabled()) {
        std::cerr << "[ScarlettX][xg_xs] XGCreateTexture2DComputer(" << desc->Width << "x"
                  << desc->Height << ", mips=" << desc->MipLevels << ", fmt=" << desc->Format
                  << ", array=" << desc->ArraySize << ")\n";
    }
    auto* obj = new XGTexture2DComputerImpl();
    obj->lpVtbl = &kVtbl;
    obj->desc = *desc;
    ComputeLayout(obj->desc, obj->layout);
    *computer = obj;
    return 0;  // S_OK
}

}  // extern "C"
