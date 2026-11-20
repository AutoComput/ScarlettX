#define SCARLETTX_BUILD_BVH
#include "scarlettx/gpu/d3d12_xs_abi.h"

#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "scarlettx/gpu/rt_bvh.h"

namespace {

struct BlasEntry {
    std::vector<scarlettx::RtTriangle> triangles;
};

struct TlasEntry {
    scarlettx::BvhAccelerationStructure bvh;
};

std::mutex g_bvh_mutex;
uint64_t g_next_bvh_handle = 0xB7000001ull;
std::unordered_map<uint64_t, BlasEntry> g_blas_map;
std::unordered_map<uint64_t, std::unique_ptr<TlasEntry>> g_tlas_map;

}  // namespace

extern "C" {

int32_t XboxBvhBuildBlas(const SCARLETT_BVH_BUILD_DESC* desc, uint64_t* out_blas_handle) {
    if (!desc || !desc->Triangles || desc->TriangleCount == 0 || !out_blas_handle) {
        return -1;
    }
    BlasEntry entry;
    entry.triangles.reserve(desc->TriangleCount);
    for (uint32_t i = 0; i < desc->TriangleCount; ++i) {
        const auto& src = desc->Triangles[i];
        scarlettx::RtTriangle tri{};
        tri.v0 = {src.v0[0], src.v0[1], src.v0[2]};
        tri.v1 = {src.v1[0], src.v1[1], src.v1[2]};
        tri.v2 = {src.v2[0], src.v2[1], src.v2[2]};
        tri.color = {src.color[0], src.color[1], src.color[2]};
        tri.reflectivity = src.reflectivity;
        entry.triangles.push_back(tri);
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const uint64_t handle = g_next_bvh_handle++;
    g_blas_map[handle] = std::move(entry);
    *out_blas_handle = handle;
    return 0;
}

int32_t XboxBvhBuildTlas(const uint64_t* blas_handles,
                         uint32_t blas_count,
                         uint64_t* out_tlas_handle) {
    if (!blas_handles || blas_count == 0 || !out_tlas_handle) {
        return -1;
    }

    std::vector<scarlettx::RtTriangle> combined;
    {
        std::lock_guard<std::mutex> lock(g_bvh_mutex);
        for (uint32_t i = 0; i < blas_count; ++i) {
            auto it = g_blas_map.find(blas_handles[i]);
            if (it == g_blas_map.end()) {
                return -2;
            }
            combined.insert(combined.end(),
                            it->second.triangles.begin(),
                            it->second.triangles.end());
        }
    }

    auto tlas = std::make_unique<TlasEntry>();
    auto st = tlas->bvh.Build(combined);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    const uint64_t handle = g_next_bvh_handle++;
    g_tlas_map[handle] = std::move(tlas);
    *out_tlas_handle = handle;
    return 0;
}

int32_t XboxBvhDispatchRays(uint64_t tlas_handle,
                            const SCARLETT_RT_DISPATCH_DESC* desc,
                            const char* optional_ppm_path,
                            uint64_t* out_frame_hash) {
    if (!desc) {
        return -1;
    }

    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    auto it = g_tlas_map.find(tlas_handle);
    if (it == g_tlas_map.end()) {
        return -2;
    }

    scarlettx::RtDispatchParams params{};
    params.width = desc->Width;
    params.height = desc->Height;
    params.camera_pos = {desc->CameraPos[0], desc->CameraPos[1], desc->CameraPos[2]};
    params.camera_look_at = {desc->CameraLookAt[0], desc->CameraLookAt[1], desc->CameraLookAt[2]};
    params.light_pos = {desc->LightPos[0], desc->LightPos[1], desc->LightPos[2]};
    params.max_bounces = desc->MaxBounces;

    scarlettx::FrameBufferRgba8 fb;
    auto st = it->second->bvh.RenderScene(params, fb, nullptr);
    if (!st.ok()) {
        return static_cast<int32_t>(st.code);
    }
    if (optional_ppm_path && *optional_ppm_path) {
        fb.WritePpm(optional_ppm_path);
    }
    if (out_frame_hash) {
        *out_frame_hash = fb.ComputeFnv1aHash();
    }
    return 0;
}

int32_t XboxBvhDestroy(uint64_t handle) {
    std::lock_guard<std::mutex> lock(g_bvh_mutex);
    if (g_blas_map.erase(handle) > 0 || g_tlas_map.erase(handle) > 0) {
        return 0;
    }
    return -2;
}

}  // extern "C"
