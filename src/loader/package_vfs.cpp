#include "scarlettx/loader/package_vfs.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace scarlettx {

std::string PackageVfs::NormalizeVirtualPath(const std::string& path) {
    std::string out = path;
    std::replace(out.begin(), out.end(), '/', '\\');
    return out;
}

Status PackageVfs::MountDirectory(const std::string& virtual_root, const std::string& host_dir) {
    if (virtual_root.empty() || host_dir.empty()) {
        return Status::Error(StatusCode::InvalidArgument, "Empty virtual_root or host_dir");
    }
    if (!std::filesystem::exists(host_dir)) {
        return Status::Error(StatusCode::NotFound, "Host directory does not exist: " + host_dir);
    }
    std::string root = NormalizeVirtualPath(virtual_root);
    while (!root.empty() && root.back() == '\\') {
        root.pop_back();
    }
    std::transform(root.begin(), root.end(), root.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    mounts_[root] = host_dir;
    return Status::Ok();
}

Status PackageVfs::ResolvePath(const std::string& virtual_path, std::string& out_host_path) const {
    std::string norm = NormalizeVirtualPath(virtual_path);
    std::string lower = norm;
    std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });

    for (const auto& [root, host_dir] : mounts_) {
        if (lower.rfind(root, 0) == 0 &&
            (lower.size() == root.size() || lower[root.size()] == '\\')) {
            std::string rel = norm.substr(root.size());
            while (!rel.empty() && rel.front() == '\\') {
                rel.erase(rel.begin());
            }
            std::filesystem::path p = std::filesystem::path(host_dir) / rel;
            out_host_path = p.string();
            return Status::Ok();
        }
    }
    return Status::Error(StatusCode::NotFound, "No mount point for virtual path: " + virtual_path);
}

Status PackageVfs::ReadFileBytes(const std::string& virtual_path,
                                 std::vector<uint8_t>& out_data) const {
    std::string host_path;
    Status st = ResolvePath(virtual_path, host_path);
    if (!st.ok()) {
        return st;
    }
    std::ifstream ifs(host_path, std::ios::binary);
    if (!ifs) {
        return Status::Error(StatusCode::IoError, "Failed to open mounted file: " + host_path);
    }
    out_data.assign((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return Status::Ok();
}

bool PackageVfs::Exists(const std::string& virtual_path) const {
    std::string host_path;
    if (!ResolvePath(virtual_path, host_path).ok()) {
        return false;
    }
    return std::filesystem::exists(host_path);
}

}  // namespace scarlettx
