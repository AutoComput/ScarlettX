#include <cassert>
#include <filesystem>
#include <iostream>

#include "scarlettx/gpu/d3d12_backend.h"
#include "scarlettx/gpu/shader_ir.h"

int main() {
    scarlettx::D3D12Backend backend;
    auto st = backend.Initialize(true, false);
    assert(st.ok());
    assert(backend.IsInitialized());

    const auto& info = backend.AdapterInfo();
    std::cout << "[test_d3d12_warp] Adapter=\"" << info.description
              << "\" WARP=" << (info.is_warp ? "yes" : "no")
              << " Device5=" << (info.supports_device5 ? "yes" : "no")
              << " DXR_Tier=" << info.raytracing_tier << "\n";

    scarlettx::FrameBufferRgba8 fb1;
    st = backend.RenderTestTriangle(256, 256, fb1);
    assert(st.ok());
    assert(fb1.pixels.size() == 256u * 256u * 4u);

    // Center pixel must be inside the rendered triangle (not the dark blue clear color).
    const size_t center_idx = (128u * 256u + 128u) * 4u;
    const uint8_t cr = fb1.pixels[center_idx + 0];
    const uint8_t cg = fb1.pixels[center_idx + 1];
    const uint8_t cb = fb1.pixels[center_idx + 2];
    assert(cr > 60 || cg > 60 || cb > 60);

    // Corner pixel (0,0) must match the dark clear color.
    assert(fb1.pixels[0] < 30 && fb1.pixels[1] < 35 && fb1.pixels[2] < 50);

    // Second render must produce the exact same deterministic FNV-1a hash.
    scarlettx::FrameBufferRgba8 fb2;
    st = backend.RenderTestTriangle(256, 256, fb2);
    assert(st.ok());
    const uint64_t h1 = fb1.ComputeFnv1aHash();
    const uint64_t h2 = fb2.ComputeFnv1aHash();
    assert(h1 == h2);

    const auto ppm_path = std::filesystem::path(SCARLETTX_BINARY_DIR) / "test_d3d12_warp_tri.ppm";
    assert(fb1.WritePpm(ppm_path.string()).ok());
    assert(std::filesystem::exists(ppm_path));

    // Shader container inspection check (synthetic DXBC/DXIL header with RDAT chunk)
    std::vector<uint8_t> fake_dxil(64, 0);
    fake_dxil[0] = 'D'; fake_dxil[1] = 'X'; fake_dxil[2] = 'B'; fake_dxil[3] = 'C';
    fake_dxil[28] = 2;  // 2 chunks
    fake_dxil[32] = 40; // chunk 0 offset
    fake_dxil[36] = 48; // chunk 1 offset
    fake_dxil[40] = 'D'; fake_dxil[41] = 'X'; fake_dxil[42] = 'I'; fake_dxil[43] = 'L';
    fake_dxil[48] = 'R'; fake_dxil[49] = 'D'; fake_dxil[50] = 'A'; fake_dxil[51] = 'T';

    scarlettx::ShaderModuleInfo sh_info;
    assert(scarlettx::InspectShaderContainer(fake_dxil, sh_info).ok());
    assert(sh_info.kind == scarlettx::ShaderContainerKind::Dxil);
    assert(sh_info.has_raytracing_exports);

    std::cout << "[test_d3d12_warp] PASSED (hash=0x" << std::hex << h1 << std::dec << ")\n";
    return 0;
}
