#include <cassert>
#include <filesystem>
#include <iostream>

#include "scarlettx/gpu/rt_bvh.h"

int main() {
    auto scene = scarlettx::BuildDefaultRtTestScene();
    assert(scene.size() == 12);

    scarlettx::BvhAccelerationStructure bvh;
    auto st = bvh.Build(scene);
    assert(st.ok());
    assert(bvh.TriangleCount() == 12);
    assert(bvh.NodeCount() > 1);

    scarlettx::RtDispatchParams params{};
    params.width = 160;
    params.height = 120;
    params.max_bounces = 2;

    scarlettx::FrameBufferRgba8 frame1;
    scarlettx::RtStats stats1{};
    st = bvh.RenderScene(params, frame1, &stats1);
    assert(st.ok());
    assert(stats1.primary_rays == 160u * 120u);
    assert(stats1.shadow_rays > 0);
    assert(stats1.reflection_rays > 0);
    assert(stats1.aabb_tests > 0);
    assert(stats1.triangle_tests > 0);

    // Determinism check: second RT dispatch must produce identical pixel hash.
    scarlettx::FrameBufferRgba8 frame2;
    st = bvh.RenderScene(params, frame2, nullptr);
    assert(st.ok());
    const uint64_t h1 = frame1.ComputeFnv1aHash();
    const uint64_t h2 = frame2.ComputeFnv1aHash();
    assert(h1 == h2);

    const auto ppm_path = std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_rt_pipeline_frame.ppm";
    assert(frame1.WritePpm(ppm_path.string()).ok());
    assert(std::filesystem::exists(ppm_path));

    std::cout << "[test_rt_pipeline] PASSED (nodes=" << stats1.bvh_nodes
              << ", primary=" << stats1.primary_rays
              << ", shadow=" << stats1.shadow_rays
              << ", refl=" << stats1.reflection_rays
              << ", hash=0x" << std::hex << h1 << std::dec << ")\n";
    return 0;
}
