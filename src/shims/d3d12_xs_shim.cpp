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
#include "scarlettx/gpu/shader_ir.h"

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

struct RootSignatureEntry {
    D3D12XBOX_ROOT_SIGNATURE_DESC desc{};
    int32_t constants_param_idx = -1;
    int32_t uav0_param_idx = -1;
#if defined(_WIN32)
    ID3D12RootSignature* native = nullptr;
    ~RootSignatureEntry() {
        if (native) native->Release();
    }
#endif
};

struct DescriptorHeapEntry {
    D3D12XBOX_DESCRIPTOR_HEAP_DESC desc{};
#if defined(_WIN32)
    ID3D12DescriptorHeap* native = nullptr;
    ~DescriptorHeapEntry() {
        if (native) native->Release();
    }
#endif
};

struct PipelineStateEntry {
    bool is_compute = false;
    bool is_raytracing = false;
    uint64_t root_sig_handle = 0;
    uint32_t rtv_format = 28;  // DXGI_FORMAT_R8G8B8A8_UNORM
    uint64_t pso_key = 0;
    uint32_t max_payload_bytes = 0;
    uint32_t max_attribute_bytes = 0;
    uint32_t max_recursion_depth = 0;
#if defined(_WIN32)
    ID3D12PipelineState* native = nullptr;
    ~PipelineStateEntry() {
        if (native) native->Release();
    }
#endif
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

    uint64_t next_root_sig_handle = 0x21000001ull;
    uint64_t next_heap_handle = 0x22000001ull;
    uint64_t next_pso_handle = 0x23000001ull;
    std::unordered_map<uint64_t, std::unique_ptr<RootSignatureEntry>> root_signatures;
    std::unordered_map<uint64_t, std::unique_ptr<DescriptorHeapEntry>> descriptor_heaps;
    std::unordered_map<uint64_t, std::unique_ptr<PipelineStateEntry>> psos;
    uint64_t pso_cache_hits = 0;
    uint64_t pso_cache_misses = 0;
#if defined(_WIN32)
    std::unordered_map<uint64_t, ID3D12PipelineState*> pso_cache;
    ~D3D12XsDeviceState() {
        for (auto& kv : pso_cache) {
            if (kv.second) kv.second->Release();
        }
        pso_cache.clear();
        psos.clear();
        descriptor_heaps.clear();
        root_signatures.clear();
    }
#endif
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

// ---------------------------------------------------------------------------
// Milestone 6: Shader Inspection, Root Signatures, Descriptor Heaps & PSOs
// ---------------------------------------------------------------------------
int32_t D3D12XboxInspectShader(
    const D3D12XBOX_SHADER_BYTECODE* shader,
    D3D12XBOX_SHADER_INSPECTION* out_info) {
    if (!shader || !shader->Bytecode || shader->BytecodeLength == 0 || !out_info) {
        return -1;
    }
    std::memset(out_info, 0, sizeof(*out_info));
    const auto* bytes = static_cast<const uint8_t*>(shader->Bytecode);
    std::vector<uint8_t> buf(bytes, bytes + shader->BytecodeLength);
    scarlettx::ShaderModuleInfo info{};
    auto st = scarlettx::InspectShaderContainer(buf, info);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    out_info->ContainerKind = static_cast<uint32_t>(info.kind);
    out_info->Stage = static_cast<uint32_t>(info.stage);
    out_info->ShaderModelMajor = info.shader_model_major;
    out_info->ShaderModelMinor = info.shader_model_minor;
    out_info->ChunkCount = info.chunk_count;
    out_info->InputParamCount = info.input_param_count;
    out_info->OutputParamCount = info.output_param_count;
    out_info->HasRaytracingExports = info.has_raytracing_exports ? 1u : 0u;
    out_info->HasEmbeddedRootSignature = info.has_embedded_root_signature ? 1u : 0u;
    out_info->HasXboxPrivateChunks = info.has_xbox_private_chunks ? 1u : 0u;
    out_info->ContentHash = info.content_hash;
    return 0;
}

int32_t D3D12XboxCreateRootSignature(
    uint64_t device_handle,
    const D3D12XBOX_ROOT_SIGNATURE_DESC* desc,
    uint64_t* out_root_sig_handle) {
    if (!desc || !out_root_sig_handle) {
        return -1;
    }
    *out_root_sig_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<RootSignatureEntry>();
    entry->desc = *desc;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev) {
        return -3;
    }
    std::vector<D3D12_ROOT_PARAMETER> params;
    if (desc->NumRootConstants32Bit > 0) {
        entry->constants_param_idx = static_cast<int32_t>(params.size());
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        p.Constants.ShaderRegister = 0;
        p.Constants.RegisterSpace = 0;
        p.Constants.Num32BitValues = desc->NumRootConstants32Bit;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumCbvs; ++i) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
        p.Descriptor.ShaderRegister = (desc->NumRootConstants32Bit > 0 ? 1u : 0u) + i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumSrvs; ++i) {
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_SRV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }
    for (uint32_t i = 0; i < desc->NumUavs; ++i) {
        if (i == 0) {
            entry->uav0_param_idx = static_cast<int32_t>(params.size());
        }
        D3D12_ROOT_PARAMETER p{};
        p.ParameterType = D3D12_ROOT_PARAMETER_TYPE_UAV;
        p.Descriptor.ShaderRegister = i;
        p.Descriptor.RegisterSpace = 0;
        p.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
        params.push_back(p);
    }

    D3D12_ROOT_SIGNATURE_DESC rs_desc{};
    rs_desc.NumParameters = static_cast<UINT>(params.size());
    rs_desc.pParameters = params.empty() ? nullptr : params.data();
    rs_desc.NumStaticSamplers = 0;
    rs_desc.pStaticSamplers = nullptr;
    rs_desc.Flags = desc->Flags
        ? static_cast<D3D12_ROOT_SIGNATURE_FLAGS>(desc->Flags)
        : D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;

    ID3DBlob* sig_blob = nullptr;
    ID3DBlob* err_blob = nullptr;
    HRESULT hr = D3D12SerializeRootSignature(
        &rs_desc, D3D_ROOT_SIGNATURE_VERSION_1, &sig_blob, &err_blob);
    if (err_blob) err_blob->Release();
    if (FAILED(hr) || !sig_blob) {
        if (sig_blob) sig_blob->Release();
        return static_cast<int32_t>(hr);
    }

    hr = native_dev->CreateRootSignature(
        0,
        sig_blob->GetBufferPointer(),
        sig_blob->GetBufferSize(),
        __uuidof(ID3D12RootSignature),
        reinterpret_cast<void**>(&entry->native));
    sig_blob->Release();
    if (FAILED(hr) || !entry->native) {
        return static_cast<int32_t>(hr);
    }
#endif

    const uint64_t handle = dev.next_root_sig_handle++;
    dev.root_signatures[handle] = std::move(entry);
    *out_root_sig_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyRootSignature(uint64_t device_handle, uint64_t root_sig_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->root_signatures.erase(root_sig_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxCreateDescriptorHeap(
    uint64_t device_handle,
    const D3D12XBOX_DESCRIPTOR_HEAP_DESC* desc,
    uint64_t* out_heap_handle) {
    if (!desc || desc->NumDescriptors == 0 || !out_heap_handle) {
        return -1;
    }
    *out_heap_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto entry = std::make_unique<DescriptorHeapEntry>();
    entry->desc = *desc;

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev) {
        return -3;
    }
    D3D12_DESCRIPTOR_HEAP_DESC hdesc{};
    hdesc.Type = static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(desc->Type);
    hdesc.NumDescriptors = desc->NumDescriptors;
    hdesc.Flags = (desc->ShaderVisible &&
                   (desc->Type == D3D12XBOX_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV ||
                    desc->Type == D3D12XBOX_DESCRIPTOR_HEAP_TYPE_SAMPLER))
                      ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE
                      : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    hdesc.NodeMask = 0;
    HRESULT hr = native_dev->CreateDescriptorHeap(
        &hdesc, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&entry->native));
    if (FAILED(hr) || !entry->native) {
        return static_cast<int32_t>(hr);
    }
#endif

    const uint64_t handle = dev.next_heap_handle++;
    dev.descriptor_heaps[handle] = std::move(entry);
    *out_heap_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyDescriptorHeap(uint64_t device_handle, uint64_t heap_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->descriptor_heaps.erase(heap_handle) > 0 ? 0 : -2;
}

uint32_t D3D12XboxGetDescriptorHandleIncrementSize(uint64_t device_handle, uint32_t heap_type) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return 0;
    }
#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(it->second->backend->NativeDevice());
    if (native_dev) {
        return native_dev->GetDescriptorHandleIncrementSize(
            static_cast<D3D12_DESCRIPTOR_HEAP_TYPE>(heap_type));
    }
#endif
    (void)heap_type;
    return 32;
}

int32_t D3D12XboxCreateGraphicsPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_GRAPHICS_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->VS.Bytecode || desc->VS.BytecodeLength == 0 ||
        !desc->PS.Bytecode || desc->PS.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto rs_it = dev.root_signatures.find(desc->RootSignatureHandle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

    std::vector<uint8_t> vs_in(
        static_cast<const uint8_t*>(desc->VS.Bytecode),
        static_cast<const uint8_t*>(desc->VS.Bytecode) + desc->VS.BytecodeLength);
    std::vector<uint8_t> ps_in(
        static_cast<const uint8_t*>(desc->PS.Bytecode),
        static_cast<const uint8_t*>(desc->PS.Bytecode) + desc->PS.BytecodeLength);

    scarlettx::ShaderTranslationResult vs_tr{};
    scarlettx::ShaderTranslationResult ps_tr{};
    if (auto st = scarlettx::TranslateXboxShaderToPc(vs_in, vs_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (auto st = scarlettx::TranslateXboxShaderToPc(ps_in, ps_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    const uint32_t rtv_fmt = desc->RtvFormat ? desc->RtvFormat : 28u;  // DXGI_FORMAT_R8G8B8A8_UNORM
    const uint64_t pso_key = vs_tr.translated_hash ^ (ps_tr.translated_hash * 1315423911ull) ^
                             (desc->RootSignatureHandle << 1) ^ static_cast<uint64_t>(rtv_fmt);

    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_compute = false;
    entry->root_sig_handle = desc->RootSignatureHandle;
    entry->rtv_format = rtv_fmt;
    entry->pso_key = pso_key;

#if defined(_WIN32)
    auto cache_it = dev.pso_cache.find(pso_key);
    if (cache_it != dev.pso_cache.end() && cache_it->second) {
        ++dev.pso_cache_hits;
        entry->native = cache_it->second;
        entry->native->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (!native_dev || !rs_it->second->native) {
            return -3;
        }
        D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs_it->second->native;
        pso_desc.VS = {vs_tr.pc_bytecode.data(), vs_tr.pc_bytecode.size()};
        pso_desc.PS = {ps_tr.pc_bytecode.data(), ps_tr.pc_bytecode.size()};
        pso_desc.BlendState.AlphaToCoverageEnable = FALSE;
        pso_desc.BlendState.IndependentBlendEnable = FALSE;
        pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
        pso_desc.SampleMask = UINT_MAX;
        pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        pso_desc.RasterizerState.DepthClipEnable = TRUE;
        pso_desc.DepthStencilState.DepthEnable = FALSE;
        pso_desc.DepthStencilState.StencilEnable = FALSE;
        pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        pso_desc.NumRenderTargets = 1;
        pso_desc.RTVFormats[0] = static_cast<DXGI_FORMAT>(rtv_fmt);
        pso_desc.SampleDesc.Count = desc->SampleCount ? desc->SampleCount : 1u;

        HRESULT hr = native_dev->CreateGraphicsPipelineState(
            &pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->native));
        if (FAILED(hr) || !entry->native) {
            return static_cast<int32_t>(hr);
        }
        entry->native->AddRef();
        dev.pso_cache[pso_key] = entry->native;
    }
#endif

    const uint64_t handle = dev.next_pso_handle++;
    dev.psos[handle] = std::move(entry);
    *out_pso_handle = handle;
    return 0;
}

int32_t D3D12XboxCreateComputePipelineState(
    uint64_t device_handle,
    const D3D12XBOX_COMPUTE_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->CS.Bytecode || desc->CS.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto rs_it = dev.root_signatures.find(desc->RootSignatureHandle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

    std::vector<uint8_t> cs_in(
        static_cast<const uint8_t*>(desc->CS.Bytecode),
        static_cast<const uint8_t*>(desc->CS.Bytecode) + desc->CS.BytecodeLength);
    scarlettx::ShaderTranslationResult cs_tr{};
    if (auto st = scarlettx::TranslateXboxShaderToPc(cs_in, cs_tr); !st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    const uint64_t pso_key = cs_tr.translated_hash ^ (desc->RootSignatureHandle << 3) ^ 0xC0DE0001ull;
    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_compute = true;
    entry->root_sig_handle = desc->RootSignatureHandle;
    entry->pso_key = pso_key;

#if defined(_WIN32)
    auto cache_it = dev.pso_cache.find(pso_key);
    if (cache_it != dev.pso_cache.end() && cache_it->second) {
        ++dev.pso_cache_hits;
        entry->native = cache_it->second;
        entry->native->AddRef();
    } else {
        ++dev.pso_cache_misses;
        auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
        if (!native_dev || !rs_it->second->native) {
            return -3;
        }
        D3D12_COMPUTE_PIPELINE_STATE_DESC pso_desc{};
        pso_desc.pRootSignature = rs_it->second->native;
        pso_desc.CS = {cs_tr.pc_bytecode.data(), cs_tr.pc_bytecode.size()};
        HRESULT hr = native_dev->CreateComputePipelineState(
            &pso_desc, __uuidof(ID3D12PipelineState), reinterpret_cast<void**>(&entry->native));
        if (FAILED(hr) || !entry->native) {
            return static_cast<int32_t>(hr);
        }
        entry->native->AddRef();
        dev.pso_cache[pso_key] = entry->native;
    }
#endif

    const uint64_t handle = dev.next_pso_handle++;
    dev.psos[handle] = std::move(entry);
    *out_pso_handle = handle;
    return 0;
}

int32_t D3D12XboxDestroyPipelineState(uint64_t device_handle, uint64_t pso_handle) {
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    return it->second->psos.erase(pso_handle) > 0 ? 0 : -2;
}

int32_t D3D12XboxDispatchCompute(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t group_x,
    uint32_t group_y,
    uint32_t group_z,
    const uint32_t* root_constants,
    uint32_t root_constant_count,
    void* out_uav_data,
    uint32_t uav_byte_size) {
    if (group_x == 0 || group_y == 0 || group_z == 0 || !out_uav_data || uav_byte_size == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(pso_handle);
    if (pso_it == dev.psos.end() || !pso_it->second->is_compute) {
        return -2;
    }
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) {
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12Resource* uav_buf = nullptr;
    ID3D12Resource* rb_buf = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb_buf) rb_buf->Release();
        if (uav_buf) uav_buf->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
    };

    const uint64_t aligned_bytes = AlignUp(uav_byte_size, 256);

    HRESULT hr = native_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC uav_desc{};
    uav_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    uav_desc.Width = aligned_bytes;
    uav_desc.Height = 1;
    uav_desc.DepthOrArraySize = 1;
    uav_desc.MipLevels = 1;
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.SampleDesc.Count = 1;
    uav_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    uav_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    hr = native_dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &uav_desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&uav_buf));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc = uav_desc;
    rb_desc.Flags = D3D12_RESOURCE_FLAG_NONE;
    hr = native_dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb_buf));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    cmd->SetComputeRootSignature(rs_it->second->native);
    if (rs_it->second->constants_param_idx >= 0 && root_constants && root_constant_count > 0) {
        const uint32_t n = std::min(root_constant_count, rs_it->second->desc.NumRootConstants32Bit);
        cmd->SetComputeRoot32BitConstants(
            static_cast<UINT>(rs_it->second->constants_param_idx), n, root_constants, 0);
    }
    if (rs_it->second->uav0_param_idx >= 0) {
        cmd->SetComputeRootUnorderedAccessView(
            static_cast<UINT>(rs_it->second->uav0_param_idx), uav_buf->GetGPUVirtualAddress());
    }
    cmd->Dispatch(group_x, group_y, group_z);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = uav_buf;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);
    cmd->CopyBufferRegion(rb_buf, 0, uav_buf, 0, uav_byte_size);

    hr = cmd->Close();
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);

    hr = native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { cleanup(); return -4; }
    hr = queue->Signal(fence, 1);
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    D3D12_RANGE range{0, uav_byte_size};
    hr = rb_buf->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) { cleanup(); return static_cast<int32_t>(hr); }
    std::memcpy(out_uav_data, mapped, uav_byte_size);
    D3D12_RANGE empty_range{0, 0};
    rb_buf->Unmap(0, &empty_range);

    cleanup();
    return 0;
#else
    (void)root_constants;
    (void)root_constant_count;
    return -3;
#endif
}

int32_t D3D12XboxRenderWithPipelineState(
    uint64_t device_handle,
    uint64_t pso_handle,
    uint32_t width,
    uint32_t height,
    uint32_t vertex_count,
    const char* optional_ppm_path,
    uint64_t* out_pixel_hash) {
    if (width == 0 || height == 0 || vertex_count == 0) {
        return -1;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    auto pso_it = dev.psos.find(pso_handle);
    if (pso_it == dev.psos.end() || pso_it->second->is_compute) {
        return -2;
    }
    auto rs_it = dev.root_signatures.find(pso_it->second->root_sig_handle);
    if (rs_it == dev.root_signatures.end()) {
        return -2;
    }

#if defined(_WIN32)
    auto* native_dev = static_cast<ID3D12Device*>(dev.backend->NativeDevice());
    if (!native_dev || !pso_it->second->native || !rs_it->second->native) {
        return -3;
    }

    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* cmd = nullptr;
    ID3D12DescriptorHeap* rtv_heap = nullptr;
    ID3D12Resource* rt = nullptr;
    ID3D12Resource* rb = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;

    auto cleanup = [&]() {
        if (ev) CloseHandle(ev);
        if (fence) fence->Release();
        if (rb) rb->Release();
        if (rt) rt->Release();
        if (rtv_heap) rtv_heap->Release();
        if (cmd) cmd->Release();
        if (alloc) alloc->Release();
        if (queue) queue->Release();
    };

    HRESULT hr = native_dev->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), reinterpret_cast<void**>(&queue));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator), reinterpret_cast<void**>(&alloc));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    hr = native_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, pso_it->second->native, __uuidof(ID3D12GraphicsCommandList), reinterpret_cast<void**>(&cmd));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    hd.NumDescriptors = 1;
    hr = native_dev->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), reinterpret_cast<void**>(&rtv_heap));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_HEAP_PROPERTIES def_heap{};
    def_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rt_desc{};
    rt_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rt_desc.Width = width;
    rt_desc.Height = height;
    rt_desc.DepthOrArraySize = 1;
    rt_desc.MipLevels = 1;
    rt_desc.Format = static_cast<DXGI_FORMAT>(pso_it->second->rtv_format);
    rt_desc.SampleDesc.Count = 1;
    rt_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    D3D12_CLEAR_VALUE cv{};
    cv.Format = rt_desc.Format;
    cv.Color[0] = 0.05f;
    cv.Color[1] = 0.07f;
    cv.Color[2] = 0.11f;
    cv.Color[3] = 1.0f;

    hr = native_dev->CreateCommittedResource(
        &def_heap, D3D12_HEAP_FLAG_NONE, &rt_desc,
        D3D12_RESOURCE_STATE_RENDER_TARGET, &cv,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rt));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_CPU_DESCRIPTOR_HANDLE rtv_handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    native_dev->CreateRenderTargetView(rt, nullptr, rtv_handle);

    const uint64_t row_pitch = AlignUp(static_cast<uint64_t>(width) * 4ull, D3D12_TEXTURE_DATA_PITCH_ALIGNMENT);
    const uint64_t rb_size = row_pitch * height;
    D3D12_HEAP_PROPERTIES rb_heap{};
    rb_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC rb_desc{};
    rb_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rb_desc.Width = rb_size;
    rb_desc.Height = 1;
    rb_desc.DepthOrArraySize = 1;
    rb_desc.MipLevels = 1;
    rb_desc.Format = DXGI_FORMAT_UNKNOWN;
    rb_desc.SampleDesc.Count = 1;
    rb_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    hr = native_dev->CreateCommittedResource(
        &rb_heap, D3D12_HEAP_FLAG_NONE, &rb_desc,
        D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
        __uuidof(ID3D12Resource), reinterpret_cast<void**>(&rb));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }

    D3D12_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    D3D12_RECT sc{0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};
    cmd->SetGraphicsRootSignature(rs_it->second->native);
    cmd->RSSetViewports(1, &vp);
    cmd->RSSetScissorRects(1, &sc);
    cmd->OMSetRenderTargets(1, &rtv_handle, FALSE, nullptr);
    cmd->ClearRenderTargetView(rtv_handle, cv.Color, 0, nullptr);
    cmd->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    cmd->DrawInstanced(vertex_count, 1, 0, 0);

    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = rt;
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cmd->ResourceBarrier(1, &barrier);

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb;
    dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = rt_desc.Format;
    dst.PlacedFootprint.Footprint.Width = width;
    dst.PlacedFootprint.Footprint.Height = height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = static_cast<UINT>(row_pitch);

    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = rt;
    src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cmd->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    hr = cmd->Close();
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ID3D12CommandList* lists[] = {cmd};
    queue->ExecuteCommandLists(1, lists);

    hr = native_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, __uuidof(ID3D12Fence), reinterpret_cast<void**>(&fence));
    if (FAILED(hr)) { cleanup(); return static_cast<int32_t>(hr); }
    ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    if (!ev) { cleanup(); return -4; }
    queue->Signal(fence, 1);
    if (fence->GetCompletedValue() < 1) {
        fence->SetEventOnCompletion(1, ev);
        WaitForSingleObject(ev, 5000);
    }

    void* mapped = nullptr;
    D3D12_RANGE range{0, static_cast<SIZE_T>(rb_size)};
    hr = rb->Map(0, &range, &mapped);
    if (FAILED(hr) || !mapped) { cleanup(); return static_cast<int32_t>(hr); }

    scarlettx::FrameBufferRgba8 frame{};
    frame.width = width;
    frame.height = height;
    frame.pixels.resize(static_cast<size_t>(width) * height * 4ull);
    const auto* src_bytes = static_cast<const uint8_t*>(mapped);
    for (uint32_t y = 0; y < height; ++y) {
        std::memcpy(frame.pixels.data() + static_cast<size_t>(y) * width * 4ull,
                    src_bytes + static_cast<size_t>(y) * row_pitch,
                    static_cast<size_t>(width) * 4ull);
    }
    D3D12_RANGE empty_range{0, 0};
    rb->Unmap(0, &empty_range);

    if (out_pixel_hash) {
        *out_pixel_hash = frame.ComputeFnv1aHash();
    }
    if (optional_ppm_path && optional_ppm_path[0] != '\0') {
        auto st = frame.WritePpm(optional_ppm_path);
        if (!st.ok()) {
            cleanup();
            return static_cast<int32_t>(st.code);
        }
    }
    cleanup();
    return 0;
#else
    (void)optional_ppm_path;
    (void)out_pixel_hash;
    return -3;
#endif
}

int32_t D3D12XboxCreateRaytracingPipelineState(
    uint64_t device_handle,
    const D3D12XBOX_RAYTRACING_PSO_DESC* desc,
    uint64_t* out_pso_handle) {
    if (!desc || !out_pso_handle || !desc->DxilLibrary.Bytecode ||
        desc->DxilLibrary.BytecodeLength == 0) {
        return -1;
    }
    *out_pso_handle = 0;

    const auto* bytes = static_cast<const uint8_t*>(desc->DxilLibrary.Bytecode);
    std::vector<uint8_t> raw(bytes, bytes + desc->DxilLibrary.BytecodeLength);
    scarlettx::ShaderTranslationResult translated{};
    auto st = scarlettx::TranslateXboxShaderToPc(raw, translated);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (translated.source_info.kind != scarlettx::ShaderContainerKind::Dxil) {
        return static_cast<int32_t>(scarlettx::StatusCode::ShaderError);
    }

    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    auto& dev = *it->second;
    if (desc->GlobalRootSignatureHandle != 0 &&
        dev.root_signatures.find(desc->GlobalRootSignatureHandle) == dev.root_signatures.end()) {
        return -2;
    }

    auto entry = std::make_unique<PipelineStateEntry>();
    entry->is_raytracing = true;
    entry->root_sig_handle = desc->GlobalRootSignatureHandle;
    entry->pso_key = translated.translated_hash ^
                     (static_cast<uint64_t>(desc->MaxPayloadSizeInBytes) << 32) ^
                     desc->MaxTraceRecursionDepth;
    entry->max_payload_bytes = desc->MaxPayloadSizeInBytes;
    entry->max_attribute_bytes = desc->MaxAttributeSizeInBytes;
    entry->max_recursion_depth = desc->MaxTraceRecursionDepth;
    // Software DXR state object backing (native ID3D12StateObject requires hardware DXR Tier >= 1.0).
    ++dev.pso_cache_misses;
    const uint64_t h = dev.next_pso_handle++;
    dev.psos.emplace(h, std::move(entry));
    *out_pso_handle = h;
    return 0;
}

int32_t D3D12XboxGetPipelineCacheStats(
    uint64_t device_handle,
    D3D12XBOX_PIPELINE_CACHE_STATS* out_stats) {
    if (!out_stats) {
        return -1;
    }
    std::memset(out_stats, 0, sizeof(*out_stats));
    std::lock_guard<std::mutex> lock(g_mutex);
    auto it = g_devices.find(device_handle);
    if (it == g_devices.end()) {
        return -2;
    }
    const auto& dev = *it->second;
    out_stats->CacheHits = dev.pso_cache_hits;
    out_stats->CacheMisses = dev.pso_cache_misses;
    out_stats->LiveRootSignatures = static_cast<uint32_t>(dev.root_signatures.size());
    out_stats->LiveDescriptorHeaps = static_cast<uint32_t>(dev.descriptor_heaps.size());
    for (const auto& kv : dev.psos) {
        if (kv.second->is_raytracing) {
            ++out_stats->LiveRaytracingPsos;
        } else if (kv.second->is_compute) {
            ++out_stats->LiveComputePsos;
        } else {
            ++out_stats->LiveGraphicsPsos;
        }
    }
    return 0;
}

}  // extern "C"
