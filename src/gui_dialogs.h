#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "password.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace poolfs::gui {

// The password is collected only in masked controls and returned in a
// zeroing Secret. Nothing is written to a command line or config file.
std::optional<password::Secret> prompt_password(HWND parent, bool confirm);

std::optional<std::filesystem::path> pick_open_config(HWND parent);
std::optional<std::filesystem::path> pick_save_config(HWND parent);
std::optional<std::filesystem::path> pick_existing_image(HWND parent);
std::optional<std::filesystem::path> pick_new_image_path(HWND parent);
std::optional<std::filesystem::path> pick_folder(HWND parent);

struct PartitionOptions {
    std::uint64_t size_mib = 0;
    bool use_max = true;
    wchar_t drive_letter = L'\0';
    std::wstring filesystem = L"NTFS";
    std::wstring label;
};

// These dialogs only collect options and confirmation. They never modify a
// disk or volume. Confirmation requires an exact, case-sensitive phrase.
std::optional<PartitionOptions> prompt_partition_options(
    HWND parent, bool creating, bool change_letter);
bool confirm_partition_action(HWND parent, std::wstring_view summary,
                              std::wstring_view required_phrase);

} // namespace poolfs::gui
