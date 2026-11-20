#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

#include "scarlettx/core/status.h"

namespace scarlettx {

class PackageVfs {
public:
    Status MountDirectory(const std::string& virtual_root, const std::string& host_dir);
    Status ResolvePath(const std::string& virtual_path, std::string& out_host_path) const;
    Status ReadFileBytes(const std::string& virtual_path, std::vector<uint8_t>& out_data) const;
    bool Exists(const std::string& virtual_path) const;

private:
    static std::string NormalizeVirtualPath(const std::string& path);
    std::unordered_map<std::string, std::string> mounts_;
};

}  // namespace scarlettx
