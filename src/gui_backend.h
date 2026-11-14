#pragma once

#include "core.h"
#include "mount_config.h"
#include "password.h"

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace poolfs::gui {

struct ToolResult {
    unsigned exit_code = 0;
    std::wstring output;
};

struct VerifyResult {
    std::uint64_t capacity = 0;
    std::uint64_t available = 0;
    std::uint64_t issues = 0;
    bool incomplete_tail = false;
};

// Blocking operations are intended for the GUI's worker thread. Configs contain
// only public paths/options. Passwords stay in Secret and never enter argv,
// environment variables, JSON, or logs.
void save_mount_config(const MountConfig& config,
                       const std::filesystem::path& destination);
ToolResult discover_config(const std::filesystem::path& config_path);
bool config_is_encrypted(const MountConfig& config);
void mount_config(const std::filesystem::path& config_path,
                  const password::Secret* secret = nullptr);
void mount_config(const std::filesystem::path& config_path,
                  const MountConfig& expected,
                  const password::Secret* secret = nullptr);
bool mounted(wchar_t drive);
void unmount(wchar_t drive);
void format_v6_images(const std::vector<ImageMemberSpec>& members,
                      const password::Secret* secret = nullptr);
VerifyResult verify_pool(const MountConfig& config,
                         const password::Secret* secret = nullptr);

} // namespace poolfs::gui
