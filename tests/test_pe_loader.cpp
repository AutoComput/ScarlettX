#include <algorithm>
#include <cassert>
#include <cctype>
#include <filesystem>
#include <iostream>

#include "scarlettx/loader/pe_loader.h"

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

}  // namespace

int main() {
    scarlettx::DllRedirectionTable redirects;
    const std::filesystem::path rt_exe =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "rt_showcase" / "rt_showcase.exe";
    assert(std::filesystem::exists(rt_exe));

    scarlettx::PeImageInfo info;
    auto st = scarlettx::InspectPeFile(rt_exe.string(), redirects, info);
    assert(st.ok());
    assert(info.is_pe64);
    assert(!info.sections.empty());
    assert(!info.imports.empty());

    bool found_d3d12_xs = false;
    bool found_xgr = false;
    bool found_bvh = false;
    for (const auto& mod : info.imports) {
        const std::string name = ToLower(mod.dll_name);
        if (name == "d3d12_xs.dll") {
            found_d3d12_xs = true;
            assert(mod.is_scarlett_hle);
            assert(!mod.symbols.empty());
        } else if (name == "xgameruntime.dll") {
            found_xgr = true;
            assert(mod.is_scarlett_hle);
        } else if (name == "xboxbvhbuilder.dll") {
            found_bvh = true;
            assert(mod.is_scarlett_hle);
        }
    }

    assert(found_d3d12_xs);
    assert(found_xgr);
    assert(found_bvh);

    std::cout << "[test_pe_loader] PASSED (imports=" << info.imports.size() << ")\n";
    return 0;
}
