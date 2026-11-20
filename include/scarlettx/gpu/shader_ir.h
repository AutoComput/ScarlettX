#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

enum class ShaderContainerKind : uint32_t {
    Unknown = 0,
    Dxbc = 1,
    Dxil = 2,
    ScarlettRdna2 = 3,
};

struct ShaderModuleInfo {
    ShaderContainerKind kind = ShaderContainerKind::Unknown;
    uint32_t byte_size = 0;
    uint32_t chunk_count = 0;
    bool has_raytracing_exports = false;
    std::vector<std::string> chunk_fourccs;
};

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info);

}  // namespace scarlettx
