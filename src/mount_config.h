#pragma once

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace poolfs {

// Mount configuration v1 contains public paths and options only. A password
// must never be placed in the JSON file; encrypted pools still prompt locally.
struct MountConfig {
    wchar_t drive = L'\0';
    std::wstring name = L"PoolFS";
    std::vector<std::filesystem::path> members;
    bool require_encrypted = true;

    bool operator==(const MountConfig&) const = default;
};

// A pool name is also a Windows volume label. The limit is 32 UTF-16 units.
void validate_pool_name(std::wstring_view name);

MountConfig load_mount_config(const std::filesystem::path& config_path);

bool mount_paths_alias(const std::filesystem::path& first, const std::filesystem::path& second);
void save_mount_config(const MountConfig& config, const std::filesystem::path& destination);

} // namespace poolfs
