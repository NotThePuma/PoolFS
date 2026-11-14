#pragma once

#include <cstdint>

// Stable C ABI for the Windows GUI (including Python/Qt). All strings passed
// in are UTF-16.
// Every successful or failed call returns UTF-8 JSON in *response, allocated
// by the DLL. Call poolfs_bridge_free exactly once for a non-null response.
// Return value: 0 on success, 1 on error. Errors have {"error":"..."} shape.
// Passwords are never part of a JSON request or response.

struct PoolFsBridgePartitionRequest {
    std::uint32_t action; // InitializeGpt=0 through ChangeLetter=4.
    std::uint32_t disk_number;
    std::uint64_t expected_disk_bytes;
    const wchar_t* expected_disk_guid;
    std::uint32_t partition_number;
    const wchar_t* expected_partition_guid;
    std::uint64_t expected_partition_offset;
    std::uint64_t expected_partition_bytes;
    std::uint64_t size_mib;
    wchar_t drive_letter;
    const wchar_t* filesystem;
    const wchar_t* label;
    const wchar_t* disk_token;
};

extern "C" {

__declspec(dllexport) int __cdecl poolfs_bridge_inventory(char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_scan_images(
    const wchar_t* root_or_null, int recursive, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_load_config(
    const wchar_t* path, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_save_config(
    const wchar_t* path, wchar_t drive, const wchar_t* const* members,
    std::uint32_t count, int require_encrypted, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_save_named_config(
    const wchar_t* path, const wchar_t* name, wchar_t drive,
    const wchar_t* const* members,
    std::uint32_t count, int require_encrypted, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_config_is_encrypted(
    const wchar_t* path, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_discover_config(
    const wchar_t* path, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_format_v6(
    const wchar_t* const* paths, const std::uint64_t* sizes_bytes,
    std::uint32_t count, const wchar_t* password, std::uint32_t password_chars,
    char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_grow_v6(
    const wchar_t* const* sources, const wchar_t* const* destinations,
    const std::uint64_t* sizes_bytes, std::uint32_t count,
    const wchar_t* password, std::uint32_t password_chars,
    char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_mount_config(
    const wchar_t* path, wchar_t expected_drive,
    const wchar_t* const* expected_members, std::uint32_t count,
    int expected_require_encrypted, const wchar_t* password,
    std::uint32_t password_chars, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_mount_named_config(
    const wchar_t* path, const wchar_t* expected_name,
    wchar_t expected_drive,
    const wchar_t* const* expected_members, std::uint32_t count,
    int expected_require_encrypted, const wchar_t* password,
    std::uint32_t password_chars, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_verify_config(
    const wchar_t* path, const wchar_t* password,
    std::uint32_t password_chars, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_unmount(
    wchar_t drive, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_reclaim(
    wchar_t drive, char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_mounts(char** response) noexcept;
__declspec(dllexport) int __cdecl poolfs_bridge_partition(
    const PoolFsBridgePartitionRequest* request, int apply,
    char** response) noexcept;
__declspec(dllexport) void __cdecl poolfs_bridge_free(char* response) noexcept;

} // extern "C"
