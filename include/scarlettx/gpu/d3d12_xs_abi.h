#pragma once

#include <cstdint>

#if defined(_WIN32)
#  if defined(SCARLETTX_BUILDING_D3D12_XS)
#    define SCARLETTX_D3D12_API __declspec(dllexport)
#  else
#    define SCARLETTX_D3D12_API __declspec(dllimport)
#  endif
#  if defined(SCARLETTX_BUILDING_XGAMERUNTIME)
#    define SCARLETTX_XGR_API __declspec(dllexport)
#  else
#    define SCARLETTX_XGR_API __declspec(dllimport)
#  endif
#  if defined(SCARLETTX_BUILDING_XBOX_BVH)
#    define SCARLETTX_BVH_API __declspec(dllexport)
#  else
#    define SCARLETTX_BVH_API __declspec(dllimport)
#  endif
#else
#  define SCARLETTX_D3D12_API
#  define SCARLETTX_XGR_API
#  define SCARLETTX_BVH_API
#endif

extern "C" {

// Representative Scarlett D3D12_XS ABI structs and entry points.
struct D3D12XBOX_CREATE_DEVICE_PARAMETERS {
    uint32_t Version;
    uint32_t ProcessDebugFlags;
    uint32_t GraphicsCommandQueueRingSizeBytes;
    uint32_t GraphicsScratchMemorySizeBytes;
    uint32_t ComputeScratchMemorySizeBytes;
    uint32_t PreferWarp;
};

struct D3D12XBOX_PRESENT_PLANE_PARAMETERS {
    uint32_t Width;
    uint32_t Height;
    uint64_t Token;
};

struct D3D12XBOX_CAPABILITIES {
    uint32_t RaytracingTier;  // 0 = none, 10 = 1.0, 11 = 1.1
    uint32_t IsWarpAdapter;
    uint32_t MaxRenderWidth;
    uint32_t MaxRenderHeight;
    char AdapterName[128];
};

struct SCARLETT_BVH_TRIANGLE {
    float v0[3];
    float v1[3];
    float v2[3];
    float color[3];
    float reflectivity;
};

struct SCARLETT_BVH_BUILD_DESC {
    const SCARLETT_BVH_TRIANGLE* Triangles;
    uint32_t TriangleCount;
    uint32_t BuildFlags;
};

struct SCARLETT_RT_DISPATCH_DESC {
    uint32_t Width;
    uint32_t Height;
    float CameraPos[3];
    float CameraLookAt[3];
    float LightPos[3];
    uint32_t MaxBounces;
};

// d3d12_xs.dll exports
SCARLETTX_D3D12_API int32_t D3D12XboxCreateDevice(
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
    uint64_t* out_device_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxDestroyDevice(uint64_t device_handle);
SCARLETTX_D3D12_API int32_t D3D12XboxQueryCapabilities(
    uint64_t device_handle,
    D3D12XBOX_CAPABILITIES* out_caps);
SCARLETTX_D3D12_API int32_t D3D12XboxWaitFrameEventX(
    uint64_t device_handle,
    uint64_t* out_frame_token);
SCARLETTX_D3D12_API int32_t D3D12XboxRenderTestTriangle(
    uint64_t device_handle,
    uint32_t width,
    uint32_t height,
    const char* optional_ppm_path,
    uint64_t* out_pixel_hash);
SCARLETTX_D3D12_API int32_t D3D12XboxPresentX(
    uint64_t device_handle,
    const D3D12XBOX_PRESENT_PLANE_PARAMETERS* plane);

// xgameruntime.dll exports
SCARLETTX_XGR_API int32_t XGameRuntimeInitialize();
SCARLETTX_XGR_API void XGameRuntimeUninitialize();
SCARLETTX_XGR_API uint32_t XSystemGetDeviceType();
SCARLETTX_XGR_API int32_t XUserGetDefaultXuid(uint64_t* out_xuid);
SCARLETTX_XGR_API int32_t XTaskQueueCreate(uint64_t* out_queue_handle);
SCARLETTX_XGR_API int32_t XTaskQueueCloseHandle(uint64_t queue_handle);

// XboxBvhBuilder.dll exports
SCARLETTX_BVH_API int32_t XboxBvhBuildBlas(
    const SCARLETT_BVH_BUILD_DESC* desc,
    uint64_t* out_blas_handle);
SCARLETTX_BVH_API int32_t XboxBvhBuildTlas(
    const uint64_t* blas_handles,
    uint32_t blas_count,
    uint64_t* out_tlas_handle);
SCARLETTX_BVH_API int32_t XboxBvhDispatchRays(
    uint64_t tlas_handle,
    const SCARLETT_RT_DISPATCH_DESC* desc,
    const char* optional_ppm_path,
    uint64_t* out_frame_hash);
SCARLETTX_BVH_API int32_t XboxBvhDestroy(uint64_t handle);

}  // extern "C"
