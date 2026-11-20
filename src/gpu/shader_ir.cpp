#include "scarlettx/gpu/shader_ir.h"

#include <cstring>

namespace scarlettx {

Status InspectShaderContainer(const std::vector<uint8_t>& bytecode, ShaderModuleInfo& out_info) {
    out_info = ShaderModuleInfo{};
    out_info.byte_size = static_cast<uint32_t>(bytecode.size());

    if (bytecode.size() < 32) {
        return Status::Error(StatusCode::ShaderError, "Shader container too small (< 32 bytes)");
    }

    if (std::memcmp(bytecode.data(), "DXBC", 4) == 0) {
        uint32_t chunk_count = 0;
        std::memcpy(&chunk_count, bytecode.data() + 28, sizeof(uint32_t));
        out_info.chunk_count = chunk_count;
        out_info.kind = ShaderContainerKind::Dxbc;

        const size_t offsets_pos = 32;
        if (offsets_pos + static_cast<size_t>(chunk_count) * 4 <= bytecode.size()) {
            for (uint32_t i = 0; i < chunk_count; ++i) {
                uint32_t chunk_off = 0;
                std::memcpy(&chunk_off, bytecode.data() + offsets_pos + i * 4, sizeof(uint32_t));
                if (chunk_off + 4 <= bytecode.size()) {
                    std::string fourcc(reinterpret_cast<const char*>(bytecode.data() + chunk_off), 4);
                    if (fourcc == "DXIL" || fourcc == "ILDB") {
                        out_info.kind = ShaderContainerKind::Dxil;
                    }
                    if (fourcc == "RDAT") {
                        out_info.has_raytracing_exports = true;
                    }
                    out_info.chunk_fourccs.push_back(std::move(fourcc));
                }
            }
        }
        return Status::Ok();
    }

    if (std::memcmp(bytecode.data(), "XS2\0", 4) == 0) {
        out_info.kind = ShaderContainerKind::ScarlettRdna2;
        return Status::Ok();
    }

    return Status::Error(StatusCode::UnsupportedFormat, "Unrecognized shader container magic");
}

}  // namespace scarlettx
