#pragma once

#include <cstdint>

#if defined(_WIN32)
#if defined(SCARLETTX_BUILDING_XG_XS)
#define SCARLETTX_XG_API __declspec(dllexport)
#else
#define SCARLETTX_XG_API __declspec(dllimport)
#endif
#define SCARLETTX_XG_CALL __stdcall
#else
#define SCARLETTX_XG_API __attribute__((visibility("default")))
#define SCARLETTX_XG_CALL
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Verified Xbox Series X|S xg_xs.dll swizzle/tile mode constants.
typedef enum XG_SWIZZLE_MODE {
    XG_SWIZZLE_MODE_LINEAR = 0,
    XG_SWIZZLE_MODE_256B_S = 1,
    XG_SWIZZLE_MODE_256B_D = 2,
    XG_SWIZZLE_MODE_256B_R = 3,
    XG_SWIZZLE_MODE_4KB_Z = 4,
    XG_SWIZZLE_MODE_4KB_S = 5,
    XG_SWIZZLE_MODE_4KB_D = 6,
    XG_SWIZZLE_MODE_4KB_R = 7,
    XG_SWIZZLE_MODE_64KB_Z = 8,
    XG_SWIZZLE_MODE_64KB_S = 9,
    XG_SWIZZLE_MODE_64KB_D = 10,
    XG_SWIZZLE_MODE_64KB_R = 11,
    XG_SWIZZLE_MODE_64KB_Z_T = 16,
    XG_SWIZZLE_MODE_64KB_S_T = 17,
    XG_SWIZZLE_MODE_64KB_D_T = 18,
    XG_SWIZZLE_MODE_64KB_R_T = 19,
    XG_SWIZZLE_MODE_4KB_Z_X = 20,
    XG_SWIZZLE_MODE_4KB_S_X = 21,
    XG_SWIZZLE_MODE_4KB_D_X = 22,
    XG_SWIZZLE_MODE_4KB_R_X = 23,
    XG_SWIZZLE_MODE_64KB_Z_X = 24,
    XG_SWIZZLE_MODE_64KB_S_X = 25,
    XG_SWIZZLE_MODE_64KB_D_X = 26,
    XG_SWIZZLE_MODE_64KB_R_X = 27
} XG_SWIZZLE_MODE;

typedef struct XG_SAMPLE_DESC {
    uint32_t Count;
    uint32_t Quality;
} XG_SAMPLE_DESC;

typedef struct XG_TEXTURE2D_DESC {
    uint32_t Width;
    uint32_t Height;
    uint32_t MipLevels;
    uint32_t ArraySize;
    uint32_t Format;
    XG_SAMPLE_DESC SampleDesc;
    uint32_t Usage;
    uint32_t BindFlags;
    uint32_t CPUAccessFlags;
    uint32_t MiscFlags;
    uint32_t ESRAMOffsetBytes;
    uint32_t ESRAMUsageBytes;
    uint32_t TileMode;
    uint32_t Pitch;
} XG_TEXTURE2D_DESC;

typedef struct XG_MIPLEVEL_LAYOUT {
    uint64_t SizeBytes;
    uint64_t OffsetBytes;
    uint64_t Slice2DSizeBytes;
    uint32_t PitchPixels;
    uint32_t PitchBytes;
    uint32_t AlignmentBytes;
    uint32_t PaddedWidthElements;
    uint32_t PaddedHeightElements;
    uint32_t PaddedDepthOrArraySize;
    uint32_t WidthElements;
    uint32_t HeightElements;
    uint32_t DepthOrArraySize;
    uint32_t SampleCount;
    uint32_t TileMode;
    uint32_t BankRotationAddressBitMask;
    uint32_t BankRotationBytesPerSlice;
    uint32_t SliceDepthElements;
} XG_MIPLEVEL_LAYOUT;

typedef struct XG_PLANE_LAYOUT {
    uint32_t Usage;
    uint64_t SizeBytes;
    uint64_t BaseOffsetBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t BytesPerElement;
    XG_MIPLEVEL_LAYOUT MipLayout[15];
} XG_PLANE_LAYOUT;

typedef struct XG_RESOURCE_LAYOUT {
    uint64_t SizeBytes;
    uint64_t BaseAlignmentBytes;
    uint32_t MipLevels;
    uint32_t Planes;
    XG_PLANE_LAYOUT Plane[4];
    uint32_t Dimension;
} XG_RESOURCE_LAYOUT;

// Verified exports from EmbeddedXvd/Windows/System32/xg_xs.dll
SCARLETTX_XG_API uint32_t SCARLETTX_XG_CALL XGComputeOptimalSwizzleMode(
    uint32_t resourceDimension,
    uint32_t format,
    uint32_t width,
    uint32_t height,
    uint32_t depthOrArraySize,
    uint32_t sampleCount,
    uint32_t bindFlags);

SCARLETTX_XG_API int32_t SCARLETTX_XG_CALL XGCreateTexture2DComputer(
    const XG_TEXTURE2D_DESC* desc,
    void** computer);

#ifdef __cplusplus
}
#endif
