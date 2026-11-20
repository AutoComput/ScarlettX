#include "scarlettx/runtime/emulator.h"

#include <cstdlib>
#include <filesystem>
#include <iostream>

#include "scarlettx/loader/pe_mapper.h"

#if defined(_WIN32)
#include <windows.h>
#endif

namespace scarlettx {

namespace {

std::string FindGameConfigInDir(const std::string& package_dir) {
    const std::filesystem::path dir(package_dir);
    for (const char* candidate : {"MicrosoftGame.Config", "MicrosoftGame.config"}) {
        const auto p = dir / candidate;
        if (std::filesystem::exists(p)) {
            return p.string();
        }
    }
    return {};
}

}  // namespace

Emulator::Emulator(EmulatorConfig config) : config_(std::move(config)) {}

Status Emulator::InspectPackage(const std::string& package_dir,
                                PackageInspectionReport& out_report) const {
    out_report = PackageInspectionReport{};
    const std::string cfg_path = FindGameConfigInDir(package_dir);
    if (cfg_path.empty()) {
        return Status::Error(StatusCode::NotFound,
                             "MicrosoftGame.Config not found in package directory: " + package_dir);
    }

    Status st = LoadGameConfigFile(cfg_path, out_report.config);
    if (!st.ok()) {
        return st;
    }

    const GameExecutableEntry* exe = out_report.config.PrimaryExecutable();
    if (!exe) {
        return Status::Error(StatusCode::ParseError, "No primary executable in MicrosoftGame.Config");
    }

    const std::filesystem::path exe_path = std::filesystem::path(package_dir) / exe->name;
    if (!std::filesystem::exists(exe_path)) {
        return Status::Error(StatusCode::NotFound, "Package executable missing: " + exe_path.string());
    }
    out_report.executable_path = exe_path.string();

    return InspectPeFile(out_report.executable_path, redirects_, out_report.main_pe);
}

Status Emulator::BootPackage(const std::string& package_dir) {
    PackageInspectionReport report;
    Status st = InspectPackage(package_dir, report);
    if (!st.ok()) {
        return st;
    }

    if (!report.config.IsNativeScarlett()) {
        return Status::Error(StatusCode::UnsupportedFormat,
                             "Package is not a native XboxGen9/Scarlett Game Core target");
    }

#if defined(_WIN32)
    // Inject HLE shim directory at the front of PATH so guest imports of
    // d3d12_xs.dll, xgameruntime.dll, and XboxBvhBuilder.dll resolve to ScarlettX shims.
    std::string shim_dir = config_.shim_directory;
    if (shim_dir.empty()) {
        char exe_buf[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exe_buf, MAX_PATH) > 0) {
            shim_dir = std::filesystem::path(exe_buf).parent_path().string();
        }
    }

    if (!shim_dir.empty()) {
        std::string new_path = shim_dir;
        const char* cur_path = std::getenv("PATH");
        if (cur_path && *cur_path) {
            new_path += ";";
            new_path += cur_path;
        }
        SetEnvironmentVariableA("PATH", new_path.c_str());
    }

    SetEnvironmentVariableA("SCARLETTX_HEADLESS", config_.headless ? "1" : "0");
    SetEnvironmentVariableA("SCARLETTX_PREFER_WARP", config_.prefer_warp ? "1" : "0");
    if (config_.max_frames > 0) {
        SetEnvironmentVariableA("SCARLETTX_MAX_FRAMES", std::to_string(config_.max_frames).c_str());
    }
    if (!config_.rt_dump_path.empty()) {
        SetEnvironmentVariableA("SCARLETTX_RT_DUMP", config_.rt_dump_path.c_str());
    }
    if (!config_.raster_dump_path.empty()) {
        SetEnvironmentVariableA("SCARLETTX_RASTER_DUMP", config_.raster_dump_path.c_str());
    }

    if (config_.in_process) {
        MappedPeImage image;
        HleSymbolRegistry hle_registry;
        PeMapOptions map_opts;
        map_opts.force_relocate = config_.force_relocate;
        map_opts.auto_stub_missing = true;
        map_opts.shim_directory = shim_dir;

        Status map_st = image.MapFile(report.executable_path, redirects_, hle_registry, map_opts);
        if (!map_st.ok()) {
            return map_st;
        }
        std::cout << "[ScarlettX][loader] Mapped " << report.executable_path << " at 0x"
                  << std::hex << image.MappedBase() << std::dec
                  << " (relocated=" << (image.WasRelocated() ? "yes" : "no")
                  << ", relocs=" << image.RelocationsApplied()
                  << ", imports=" << image.Resolutions().size()
                  << ", stubs=" << image.Stubs().StubCount() << ")\n";

        int exit_code = 0;
        Status call_st = image.CallExportedEntry(config_.in_process_entry, exit_code);
        if (!call_st.ok()) {
            return call_st;
        }
        if (exit_code != 0) {
            return Status::Error(StatusCode::DeviceError,
                                 "In-process guest exited with non-zero code: " +
                                     std::to_string(exit_code));
        }
        return Status::Ok();
    }

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + report.executable_path + "\"";
    std::string work_dir = std::filesystem::path(report.executable_path).parent_path().string();

    if (!CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, FALSE, 0, nullptr,
                        work_dir.c_str(), &si, &pi)) {
        return Status::Error(StatusCode::DeviceError,
                             "CreateProcessA failed for guest executable: " + report.executable_path);
    }

    WaitForSingleObject(pi.hProcess, 60000);
    DWORD exit_code = 1;
    GetExitCodeProcess(pi.hProcess, &exit_code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);

    if (exit_code != 0) {
        return Status::Error(StatusCode::DeviceError,
                             "Guest process exited with non-zero code: " + std::to_string(exit_code));
    }
    return Status::Ok();
#else
    return Status::Error(StatusCode::NotImplemented, "Guest PE boot currently requires Windows");
#endif
}

}  // namespace scarlettx
