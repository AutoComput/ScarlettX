#include <algorithm>
#include <cassert>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>

#include "scarlettx/loader/pe_loader.h"
#include "scarlettx/loader/pe_mapper.h"
#include "scarlettx/runtime/emulator.h"

namespace {

std::string ToLower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

uint64_t FakeOrdinalHleHandler(uint64_t a, uint64_t b) {
    return (a * 1000ull) + b + 7ull;
}

}  // namespace

int main() {
    const std::filesystem::path probe_dir =
        std::filesystem::path(SCARLETTX_BINARY_DIR) / "packages" / "iat_probe";
    const std::filesystem::path probe_dll = probe_dir / "iat_probe.dll";
    assert(std::filesystem::exists(probe_dll));

    // 1. Verify API-set resolution helper.
    assert(scarlettx::ResolveApiSetDll("api-ms-win-crt-runtime-l1-1-0.dll") == "ucrtbase.dll");
    assert(scarlettx::ResolveApiSetDll("api-ms-win-core-processthreads-l1-1-0.dll") ==
           "kernel32.dll");
    assert(scarlettx::ResolveApiSetDll("api-ms-win-core-com-l1-1-0.dll") == "combase.dll");

    // 2. Verify static PE inspection sees both named and ordinal imports.
    scarlettx::DllRedirectionTable redirects;
    scarlettx::PeImageInfo info;
    assert(scarlettx::InspectPeFile(probe_dll.string(), redirects, info).ok());
    assert(info.is_pe64);

    bool saw_ord7 = false;
    bool saw_ord42 = false;
    bool saw_overlay_named = false;
    bool saw_xgr = false;
    for (const auto& mod : info.imports) {
        const std::string dll = ToLower(mod.dll_name);
        if (dll == "xordinal_hle.dll") {
            for (const auto& sym : mod.symbols) {
                if (sym.by_ordinal && sym.ordinal == 7) saw_ord7 = true;
            }
        } else if (dll == "xtelemetry_missing.dll") {
            for (const auto& sym : mod.symbols) {
                if (sym.by_ordinal && sym.ordinal == 42) saw_ord42 = true;
            }
        } else if (dll == "xgameoverlay_missing.dll") {
            for (const auto& sym : mod.symbols) {
                if (!sym.by_ordinal && sym.name == "XOverlayShowNotice") saw_overlay_named = true;
            }
        } else if (dll == "xgameruntime.dll") {
            saw_xgr = true;
        }
    }
    assert(saw_ord7);
    assert(saw_ord42);
    assert(saw_overlay_named);
    assert(saw_xgr);

    // 3. Map in-process with forced relocation, HLE ordinal override, and auto-stub thunks.
    scarlettx::HleSymbolRegistry hle_reg;
    hle_reg.RegisterOrdinal("xordinal_hle.dll", 7, reinterpret_cast<void*>(&FakeOrdinalHleHandler));

    scarlettx::PeMapOptions opts;
    opts.force_relocate = true;
    opts.auto_stub_missing = true;
    opts.default_stub_return = 0x55AA1234ull;
    opts.shim_directory = SCARLETTX_BINARY_DIR;

    scarlettx::MappedPeImage mapped;
    auto st = mapped.MapFile(probe_dll.string(), redirects, hle_reg, opts);
    if (!st.ok()) {
        std::cerr << "[test_inproc_loader] MapFile failed: " << st.message << "\n";
        return 1;
    }

    assert(mapped.IsMapped());
    assert(mapped.WasRelocated());
    assert(mapped.RelocationsApplied() > 0);

    // Verify resolution kinds.
    bool resolved_hle_ord = false;
    bool resolved_host_xgr = false;
    bool resolved_stub_named = false;
    bool resolved_stub_ord = false;
    for (const auto& r : mapped.Resolutions()) {
        const std::string dll = ToLower(r.dll_name);
        if (dll == "xordinal_hle.dll" && r.by_ordinal && r.ordinal == 7) {
            assert(r.kind == scarlettx::ImportResolutionKind::HleRegistry);
            resolved_hle_ord = true;
        } else if (dll == "xgameruntime.dll" && r.symbol_name == "XGameRuntimeInitialize") {
            assert(r.kind == scarlettx::ImportResolutionKind::HostDll);
            resolved_host_xgr = true;
        } else if (dll == "xgameoverlay_missing.dll" && r.symbol_name == "XOverlayShowNotice") {
            assert(r.kind == scarlettx::ImportResolutionKind::StubThunk);
            resolved_stub_named = true;
        } else if (dll == "xtelemetry_missing.dll" && r.by_ordinal && r.ordinal == 42) {
            assert(r.kind == scarlettx::ImportResolutionKind::StubThunk);
            resolved_stub_ord = true;
        }
    }
    assert(resolved_hle_ord);
    assert(resolved_host_xgr);
    assert(resolved_stub_named);
    assert(resolved_stub_ord);

    // 4. Execute relocated global pointer probe.
    using ProbeRelocFn = uint64_t (*)();
    auto probe_reloc = reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeRelocatedGlobal"));
    assert(probe_reloc != nullptr);
    assert(probe_reloc() == 0x5CA81E7700001234ull);

    // 5. Execute ordinal-imported HLE handler probe.
    using ProbeOrdFn = uint64_t (*)(uint64_t, uint64_t);
    auto probe_ord = reinterpret_cast<ProbeOrdFn>(mapped.GetExportAddress("ProbeOrdinalHle"));
    assert(probe_ord != nullptr);
    assert(probe_ord(42, 99) == 42106ull);

    // 6. Execute auto-stubbed named and ordinal imports and verify captured args + return values.
    auto probe_named_stub =
        reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeMissingNamedStub"));
    auto probe_ord_stub =
        reinterpret_cast<ProbeRelocFn>(mapped.GetExportAddress("ProbeMissingOrdinalStub"));
    assert(probe_named_stub != nullptr);
    assert(probe_ord_stub != nullptr);

    assert(probe_named_stub() == 0x55AA1234ull);
    assert(probe_ord_stub() == 0x55AA1234ull);
    assert(mapped.Stubs().TotalHitCount() == 2);

    const auto records = mapped.Stubs().SnapshotRecords();
    bool checked_named_args = false;
    bool checked_ord_args = false;
    for (const auto& rec : records) {
        const std::string dll = ToLower(rec.dll_name);
        if (dll == "xgameoverlay_missing.dll" && rec.symbol_name == "XOverlayShowNotice") {
            assert(rec.hit_count == 1);
            assert(rec.last_args[0] == 0x1111ull);
            assert(rec.last_args[1] == 0x2222ull);
            assert(rec.last_args[2] == 0x3333ull);
            assert(rec.last_args[3] == 0x4444ull);
            checked_named_args = true;
        } else if (dll == "xtelemetry_missing.dll" && rec.by_ordinal && rec.ordinal == 42) {
            assert(rec.hit_count == 1);
            assert(rec.last_args[0] == 0xAAAAull);
            assert(rec.last_args[1] == 0xBBBBull);
            assert(rec.last_args[2] == 0xCCCCull);
            assert(rec.last_args[3] == 0xDDDDull);
            checked_ord_args = true;
        }
    }
    assert(checked_named_args);
    assert(checked_ord_args);

    // 7. Verify Emulator::BootPackage with in_process=true runs ScarlettGuestMain cleanly.
    scarlettx::EmulatorConfig cfg;
    cfg.in_process = true;
    cfg.force_relocate = true;
    cfg.shim_directory = SCARLETTX_BINARY_DIR;
    scarlettx::Emulator emu(cfg);
    auto boot_st = emu.BootPackage(probe_dir.string());
    if (!boot_st.ok()) {
        std::cerr << "[test_inproc_loader] BootPackage(--in-process) failed: "
                  << boot_st.message << "\n";
        return 1;
    }

    std::cout << "[test_inproc_loader] PASSED (relocs=" << mapped.RelocationsApplied()
              << ", imports=" << mapped.Resolutions().size()
              << ", stubs=" << mapped.Stubs().StubCount() << ")\n";
    return 0;
}
