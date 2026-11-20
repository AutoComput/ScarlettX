#define SCARLETTX_BUILDING_D3D12_XS 1
#include "scarlettx/gpu/d3d12_xs_abi.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/rt_bvh.h"

#if defined(_WIN32)
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#endif

namespace {

uint32_t IntervalToHz(uint32_t interval) {
    switch (interval) {
        case D3D12XBOX_FRAME_INTERVAL_120_HZ: return 120;
        case D3D12XBOX_FRAME_INTERVAL_60_HZ:  return 60;
        case D3D12XBOX_FRAME_INTERVAL_30_HZ:  return 30;
        case D3D12XBOX_FRAME_INTERVAL_24_HZ:  return 24;
        default: return 60;
    }
}

uint64_t AlignUp(uint64_t v, uint64_t align) {
    return (v + align - 1) & ~(align - 1);
}

struct AccelerationStructureEntry {
    uint32_t type = D3D12XBOX_AS_TYPE_BOTTOM_LEVEL;
    uint32_t flags = 0;
    std::vector<scarlettx::RtTriangle> triangles;
    scarlettx::BvhAccelerationStructure bvh;
};

struct D3D12XsDeviceState {
    std::unique_ptr<scarlettx::D3D12Backend> backend;
    uint64_t next_frame_token = 1;
    uint64_t last_issued_token = 0;
    uint64_t last_presented_token = 0;
    uint64_t presented_frame_count = 0;
    uint32_t frame_interval = D3D12XBOX_FRAME_INTERVAL_60_HZ;
    uint32_t frame_interval_flags = 0;
    bool suspended = false;
    std::chrono::steady_clock::time_point last_present_time = std::chrono::steady_clock::now();
    uint64_t last_frame_duration_us = 16667;
    uint64_t next_as_handle = 0xA5000001ull;
    std::unordered_map<uint64_t, std::unique_ptr<AccelerationStructureEntry>> accel_structs;
};

std::mutex g_mutex;
uint64_t g_next_handle = 0x12000001ull;
std::unordered_map<uint64_t, std::unique_ptr<D3D12XsDeviceState>> g_devices;

}  // namespace

extern "C" {

// ---------------------------------------------------------------------------
// Standard PC D3D12 forwarding entry points
// ---------------------------------------------------------------------------
int32_t SCARLETTX_GDK_CALLBACK D3D12CreateDeviceX(
    const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
    const void* riid,
    void** ppvDevice) {
#if defined(_WIN32)
    if (!riid || !ppvDevice) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    *ppvDevice = nullptr;

    const bool prefer_warp = params ? (params->PreferWarp != 0) : true;
    const bool debug_layer =
        params ? ((params->ProcessDebugFlags & D3D12XBOX_PROCESS_DEBUG_FLAG_DEBUG) != 0) : false;

    if (debug_layer) {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(__uuidof(ID3D12Debug), reinterpret_cast<void**>(&dbg))) && dbg) {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }

    IDXGIFactory4* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(__uuidof(IDXGIFactory4), reinterpret_cast<void**>(&factory));
    if (FAILED(hr) || !factory) {
        return static_cast<int32_t>(hr);
    }

    IDXGIAdapter1* adapter = nullptr;
    if (prefer_warp) {
        hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
    } else {
        hr = factory->EnumAdapters1(0, &adapter);
        if (FAILED(hr) || !adapter) {
            hr = factory->EnumWarpAdapter(__uuidof(IDXGIAdapter1), reinterpret_cast<void**>(&adapter));
        }
    }

    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, target_iid, ppvDevice);
    if (FAILED(hr)) {
        hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, target_iid, ppvDevice);
    }

    if (adapter) adapter->Release();
    factory->Release();
    return static_cast<int32_t>(hr);
#else
    (void)params;
    (void)riid;
    (void)ppvDevice;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateDevice(
    void* pAdapter,
    uint32_t minimumFeatureLevel,
    const void* riid,
    void** ppDevice) {
#if defined(_WIN32)
    if (!riid || !ppDevice) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    return static_cast<int32_t>(D3D12CreateDevice(
        static_cast<IUnknown*>(pAdapter),
        static_cast<D3D_FEATURE_LEVEL>(minimumFeatureLevel),
        target_iid,
        ppDevice));
#else
    (void)pAdapter; (void)minimumFeatureLevel; (void)riid; (void)ppDevice;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsGetDebugInterface(const void* riid, void** ppvDebug) {
#if defined(_WIN32)
    if (!riid || !ppvDebug) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(riid);
    return static_cast<int32_t>(D3D12GetDebugInterface(target_iid, ppvDebug));
#else
    (void)riid; (void)ppvDebug;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeRootSignature(
    const void* pRootSignature,
    uint32_t version,
    void** ppBlob,
    void** ppErrorBlob) {
#if defined(_WIN32)
    if (!pRootSignature || !ppBlob) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    return static_cast<int32_t>(D3D12SerializeRootSignature(
        static_cast<const D3D12_ROOT_SIGNATURE_DESC*>(pRootSignature),
        static_cast<D3D_ROOT_SIGNATURE_VERSION>(version),
        reinterpret_cast<ID3DBlob**>(ppBlob),
        reinterpret_cast<ID3DBlob**>(ppErrorBlob)));
#else
    (void)pRootSignature; (void)version; (void)ppBlob; (void)ppErrorBlob;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsSerializeVersionedRootSignature(
    const void* pRootSignature,
    void** ppBlob,
    void** ppErrorBlob) {
#if defined(_WIN32)
    if (!pRootSignature || !ppBlob) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    return static_cast<int32_t>(D3D12SerializeVersionedRootSignature(
        static_cast<const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*>(pRootSignature),
        reinterpret_cast<ID3DBlob**>(ppBlob),
        reinterpret_cast<ID3DBlob**>(ppErrorBlob)));
#else
    (void)pRootSignature; (void)ppBlob; (void)ppErrorBlob;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsCreateRootSignatureDeserializer(
    const void* pSrcData,
    size_t srcDataSizeInBytes,
    const void* pRootSignatureDeserializerInterface,
    void** ppRootSignatureDeserializer) {
#if defined(_WIN32)
    if (!pSrcData || !pRootSignatureDeserializerInterface || !ppRootSignatureDeserializer) {
        return static_cast<int32_t>(E_INVALIDARG);
    }
    const IID target_iid = *reinterpret_cast<const IID*>(pRootSignatureDeserializerInterface);
    return static_cast<int32_t>(D3D12CreateRootSignatureDeserializer(
        pSrcData,
        srcDataSizeInBytes,
        target_iid,
        ppRootSignatureDeserializer));
#else
    (void)pSrcData; (void)srcDataSizeInBytes;
    (void)pRootSignatureDeserializerInterface; (void)ppRootSignatureDeserializer;
    return XGR_E_FAIL;
#endif
}

int32_t SCARLETTX_GDK_CALLBACK D3D12XsEnableExperimentalFeatures(
    uint32_t numFeatures,
    const void* pIIDs,
    void* pConfigurationStructs,
    uint32_t* pConfigurationStructSizes) {
#if defined(_WIN32)
    HRESULT hr = D3D12EnableExperimentalFeatures(
        numFeatures,
        static_cast<const IID*>(pIIDs),
        pConfigurationStructs,
        pConfigurationStructSizes);
    // On headless WARP/CI without developer mode, swallow E_NOINTERFACE gracefully.
    if (hr == E_NOINTERFACE) {
        return S_OK;
    }
    return static_cast<int32_t>(hr);
#else
    (void)numFeatures; (void)pIIDs; (void)pConfigurationStructs; (void)pConfigurationStructSizes;
    return XGR_S_OK;
#endif
}

// ---------------------------------------------------------------------------
// Scarlett handle-based device, frame pipeline & DXR translation exports
// ---------------------------------------------------------------------------
int32_t D3D12XboxCreateDevice(const D3D12XBOX_CREATE_DEVICE_PARAMETERS* params,
                              uint64_t* out_device_handle) {
    if (!out_device_handle) {
        return -1;
    }
    const bool prefer_warp = params ? (params->PreferWarp != 0) : true;
    const bool debug_layer = params ? (params->ProcessDebugFlags != 0) : false;

    auto state = std::make_unique<D3D12XsDeviceState>();
    state->backend = std::make_unique<scarlettx::D3D12Backend>();
    auto st = state->backend->Initialize(prefer_warp, debug_layer);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    const uint64_t handle = g_next_handle++;
    g_devices[handle] = std::move(state);
    *out_device_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyDevice(uint64_t device_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    g_devices.erase(it);
    return 0;
}

int32_t D3D12XboxGetNativeDevice(uint64_t device_handle, void** out_id3d12_device) {
    if (!out_id3d12_device) {
        return -1;
    }
    *out_id3d12_device = nullptr;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    void* dev = it->second->backend->NativeDevice();
    if (!dev) {
        return -3;
    }
#if defined(_WIN32)
    static_cast<ID3D12Device*>(dev)->AddRef();
#endif
    *out_id3d12_device = dev;
    return 0;
}

int32_t D3D12XboxQueryCapabilities(uint64_t device_handle, D3D12XBOX_CAPABILITIES* out_caps) {
    if (!out_caps) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& info = it->second->backend->AdapterInfo();
    std::memset(out_caps, 0, sizeof(*out_caps));
    out_caps->RaytracingTier = info.raytracing_tier;
    out_caps->IsWarpAdapter = info.is_warp ? 1u : 0u;
    out_caps->MaxRenderWidth = 3840;
    out_caps->MaxRenderHeight = 2160;
    std::strncpy(out_caps->AdapterName, info.description.c_str(), sizeof(out_caps->AdapterName) - 1);
    return 0;
}

int32_t D3D12XboxQueryRaytracingCapabilities(uint64_t device_handle,
                                             D3D12XBOX_RAYTRACING_CAPABILITIES* out_rt_caps) {
    if (!out_rt_caps) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& info = it->second->backend->AdapterInfo();
    std::memset(out_rt_caps, 0, sizeof(*out_rt_caps));
    out_rt_caps->HardwareRaytracingTier = info.raytracing_tier;
    out_rt_caps->ActiveBackend = (info.raytracing_tier >= 10)
        ? D3D12XBOX_RT_BACKEND_HARDWARE_DXR
        : D3D12XBOX_RT_BACKEND_SOFTWARE_BVH;
    out_rt_caps->SupportsInlineRaytracing = (info.raytracing_tier >= 11) ? 1u : 0u;
    out_rt_caps->MaxRecursionDepth = 31;
    std::strncpy(out_rt_caps->AdapterName, info.description.c_str(), sizeof(out_rt_caps->AdapterName) - 1);
    return 0;
}

int32_t D3D12XboxSetFrameIntervalX(uint64_t device_handle, uint32_t interval, uint32_t flags) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->frame_interval = interval;
    it->second->frame_interval_flags = flags;
    return 0;
}

int32_t D3D12XboxScheduleFrameEventX(uint64_t device_handle,
                                     uint32_t frame_event,
                                     uint64_t offset_us,
                                     void* event_handle) {
    (void)frame_event;
    (void)offset_us;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
#if defined(_WIN32)
    if (event_handle) {
        SetEvent(static_cast<HANDLE>(event_handle));
    }
#else
    (void)event_handle;
#endif
    return 0;
}

int32_t D3D12XboxWaitFrameEventX(uint64_t device_handle, uint64_t* out_frame_token) {
    return D3D12XboxWaitFrameEventXEx(device_handle, D3D12XBOX_FRAME_EVENT_ORIGIN, 0, out_frame_token);
}

int32_t D3D12XboxWaitFrameEventXEx(uint64_t device_handle,
                                   uint32_t frame_event,
                                   uint32_t timeout_ms,
                                   D3D12XBOX_FRAME_PIPELINE_TOKEN* out_frame_token) {
    (void)frame_event;
    (void)timeout_ms;
    if (!out_frame_token) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const uint64_t tok = it->second->next_frame_token++;
    it->second->last_issued_token = tok;
    *out_frame_token = tok;
    return 0;
}

int32_t D3D12XboxRenderTestTriangle(uint64_t device_handle,
                                    uint32_t width,
                                    uint32_t height,
                                    const char* optional_ppm_path,
                                    uint64_t* out_pixel_hash) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    scarlettx::FrameBufferRgba8 fb;
    auto st = it->second->backend->RenderTestTriangle(width, height, fb);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (optional_ppm_path && *optional_ppm_path) {
        fb.WritePpm(optional_ppm_path);
    }
    if (out_pixel_hash) {
        *out_pixel_hash = fb.ComputeFnv1aHash();
    }
    return 0;
}

int32_t D3D12XboxPresentX(uint64_t device_handle, const D3D12XBOX_PRESENT_PLANE_PARAMETERS* plane) {
    if (!plane) {
        return -1;
    }
    D3D12XBOX_PRESENT_PARAMETERS params{};
    params.Token = plane->Token;
    params.PlaneCount = 1;
    params.Flags = D3D12XBOX_PRESENT_FLAG_NONE;
    params.Planes = plane;
    params.CommandQueue = nullptr;
    return D3D12XboxPresentXEx(device_handle, &params);
}

int32_t D3D12XboxPresentXEx(uint64_t device_handle, const D3D12XBOX_PRESENT_PARAMETERS* params) {
    if (!params) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    if (dev.suspended) {
        return -4;
    }

#if defined(_WIN32)
    if (params->CommandQueue) {
        auto* queue = static_cast<ID3D12CommandQueue*>(params->CommandQueue);
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (native_dev) {
            ID3D12Fence* fence = nullptr;
            if (SUCCEEDED(native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence),
                                                  reinterpret_cast<void**>(&fence))) && fence) {
                HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                if (ev) {
                    if (SUCCEEDED(queue->Signal(fence, 1))) {
                        if (fence->GetCompletedValue() < 1) {
                            fence->SetEventOnCompletion(1, ev);
                            WaitForSingleObject(ev, 5000);
                        }
                    }
                    CloseHandle(ev);
                }
                fence->Release();
            }
        }
    }
#endif

    const auto now = std::chrono::steady_clock::now();
    const auto elapsed_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(now - dev.last_present_time).count());
    dev.last_frame_duration_us = std::max<uint64_t>(1, elapsed_us);
    dev.last_present_time = now;
    dev.last_presented_token = params->Token;
    dev.presented_frame_count++;
    return 0;
}

int32_t D3D12XboxGetFrameStatisticsX(uint64_t device_handle,
                                     D3D12XBOX_FRAME_STATISTICS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& dev = *it->second;
    out_stats->PresentedFrameCount = dev.presented_frame_count;
    out_stats->LastPresentedToken = dev.last_presented_token;
    out_stats->LastIssuedToken = dev.last_issued_token;
    out_stats->IntervalHz = IntervalToHz(dev.frame_interval);
    out_stats->Suspended = dev.suspended ? 1u : 0u;
    out_stats->LastFrameDurationUs = dev.last_frame_duration_us;
    return 0;
}

int32_t D3D12XboxSuspendX(uint64_t device_handle, uint32_t flags) {
    (void)flags;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->suspended = true;
    return 0;
}

int32_t D3D12XboxResumeX(uint64_t device_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    it->second->suspended = false;
    return 0;
}

int32_t D3D12XboxGetResourceAllocationInfo(uint64_t device_handle,
                                           uint64_t width,
                                           uint32_t height,
                                           uint32_t dxgi_format,
                                           uint64_t* out_size_bytes,
                                           uint64_t* out_alignment) {
    if (!out_size_bytes || !out_alignment || width == 0 || height == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
#if defined(_WIN32)
    auto* dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (dev) {
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Alignment = 0;
        desc.Width = width;
        desc.Height = height;
        desc.DepthOrArraySize = 1;
        desc.MipLevels = 1;
        desc.Format = static_cast<DXGI_FORMAT>(dxgi_format);
        desc.SampleDesc.Count = 1;
        desc.SampleDesc.Quality = 0;
        desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

        const D3D12_RESOURCE_ALLOCATION_INFO info = dev->GetResourceAllocationInfo(0, 1, &desc);
        *out_size_bytes = info.SizeInBytes;
        *out_alignment = info.Alignment;
        return 0;
    }
#endif
    (void)dxgi_format;
    *out_alignment = 65536;
    *out_size_bytes = AlignUp(width * static_cast<uint64_t>(height) * 4, 65536);
    return 0;
}

int32_t D3D12XboxGetRaytracingAccelerationStructurePrebuildInfo(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_INPUTS* inputs,
    D3D12XBOX_RT_AS_PREBUILD_INFO* out_info) {
    if (!inputs || !out_info) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }

#if defined(_WIN32)
    const auto& adapter = it->second->backend->AdapterInfo();
    auto* dev5 = static_cast<ID3D12Device5*>(it->second->backend->NativeDevice5());
    if (dev5 && adapter.raytracing_tier >= 10) {
        D3D12_BUILD_RAYTRACING_ACCELERATION_STRUCTURE_INPUTS dxr_inputs{};
        dxr_inputs.Type = (inputs->Type == D3D12XBOX_AS_TYPE_TOP_LEVEL)
            ? D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL
            : D3D12_RAYTRACING_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL;
        dxr_inputs.Flags = static_cast<D3D12_RAYTRACING_ACCELERATION_STRUCTURE_BUILD_FLAGS>(inputs->Flags);
        dxr_inputs.DescsLayout = D3D12_ELEMENTS_LAYOUT_ARRAY;

        D3D12_RAYTRACING_GEOMETRY_DESC geom{};
        if (inputs->Type == D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
            geom.Type = D3D12_RAYTRACING_GEOMETRY_TYPE_TRIANGLES;
            geom.Flags = D3D12_RAYTRACING_GEOMETRY_FLAG_OPAQUE;
            geom.Triangles.VertexFormat = DXGI_FORMAT_R32G32B32_FLOAT;
            geom.Triangles.VertexCount = inputs->NumDescs * 3;
            dxr_inputs.NumDescs = 1;
            dxr_inputs.pGeometryDescs = &geom;
        } else {
            dxr_inputs.NumDescs = inputs->NumDescs;
        }

        D3D12_RAYTRACING_ACCELERATION_STRUCTURE_PREBUILD_INFO info{};
        dev5->GetRaytracingAccelerationStructurePrebuildInfo(&dxr_inputs, &info);
        out_info->ResultDataMaxSizeInBytes = info.ResultDataMaxSizeInBytes;
        out_info->ScratchDataSizeInBytes = info.ScratchDataSizeInBytes;
        out_info->UpdateScratchDataSizeInBytes = info.UpdateScratchDataSizeInBytes;
        return 0;
    }
#endif

    // Deterministic Software BVH prebuild sizing (256-byte aligned like DXR)
    const uint64_t n = std::max<uint64_t>(1, inputs->NumDescs);
    const uint64_t node_bytes = (2 * n) * 64;
    const uint64_t prim_bytes = n * sizeof(SCARLETT_BVH_TRIANGLE);
    out_info->ResultDataMaxSizeInBytes = AlignUp(node_bytes + prim_bytes + 256, 256);
    out_info->ScratchDataSizeInBytes = AlignUp(n * 128 + 256, 256);
    out_info->UpdateScratchDataSizeInBytes =
        (inputs->Flags & D3D12XBOX_AS_BUILD_FLAG_ALLOW_UPDATE)
            ? AlignUp(n * 64 + 256, 256)
            : 0;
    return 0;
}

int32_t D3D12XboxBuildRaytracingAccelerationStructure(
    uint64_t device_handle,
    const D3D12XBOX_RT_AS_BUILD_DESC* desc,
    uint64_t* out_as_handle) {
    if (!desc || !out_as_handle || desc->Inputs.NumDescs == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<AccelerationStructureEntry>();
    entry->type = desc->Inputs.Type;
    entry->flags = desc->Inputs.Flags;

    if (desc->Inputs.Type == D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
        if (!desc->Triangles) {
            return -1;
        }
        entry->triangles.reserve(desc->Inputs.NumDescs);
        for (uint32_t i = 0; i < desc->Inputs.NumDescs; ++i) {
            const auto& src = desc->Triangles[i];
            scarlettx::RtTriangle tri{};
            tri.v0 = {src.v0[0], src.v0[1], src.v0[2]};
            tri.v1 = {src.v1[0], src.v1[1], src.v1[2]};
            tri.v2 = {src.v2[0], src.v2[1], src.v2[2]};
            tri.color = {src.color[0], src.color[1], src.color[2]};
            tri.reflectivity = src.reflectivity;
            entry->triangles.push_back(tri);
        }
        auto st = entry->bvh.Build(entry->triangles);
        if (!st.ok()) {
            return static_cast<int32_t>(st.code);
        }
    } else if (desc->Inputs.Type == D3D12XBOX_AS_TYPE_TOP_LEVEL) {
        if (!desc->BlasHandles) {
            return -1;
        }
        std::vector<scarlettx::RtTriangle> combined;
        for (uint32_t i = 0; i < desc->Inputs.NumDescs; ++i) {
            auto bit = dev.accel_structs.find(desc->BlasHandles[i]);
            if (bit == dev.accel_structs.end() ||
                bit->second->type != D3D12XBOX_AS_TYPE_BOTTOM_LEVEL) {
                return -2;
            }
            combined.insert(combined.end(),
                            bit->second->triangles.begin(),
                            bit->second->triangles.end());
        }
        entry->triangles = combined;
        auto st = entry->bvh.Build(entry->triangles);
        if (!st.ok()) {
            return static_cast<int32_t>(st.code);
        }
    } else {
        return -1;
    }

    const uint64_t handle = dev.next_as_handle++;
    dev.accel_structs[handle] = std::move(entry);
    *out_as_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyRaytracingAccelerationStructure(uint64_t device_handle, uint64_t as_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->accel_structs.erase(as_handle) > 0 ? 0 : -2;
}

}  // extern "C"
