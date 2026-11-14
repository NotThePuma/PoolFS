#include "gui_bridge.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>

#include "gui_backend.h"
#include "gui_inventory.h"
#include "gui_partition_ops.h"
#include "mount_config.h"
#include "password.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace gui = poolfs::gui;

std::wstring checked_text(const wchar_t* value, const char* name,
                          bool required = true) {
    if (!value) {
        if (required) throw std::invalid_argument(std::string(name) + " is missing");
        return {};
    }
    std::size_t length = 0;
    while (length < 32767 && value[length]) ++length;
    if (length == 32767)
        throw std::invalid_argument(std::string(name) + " is too long");
    if (required && length == 0)
        throw std::invalid_argument(std::string(name) + " is empty");
    return std::wstring(value, length);
}

std::wstring error_wide(std::string_view bytes) {
    if (bytes.empty()) return L"Unknown error";
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        return L"Error message is too long";
    const auto length = static_cast<int>(bytes.size());
    UINT page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int needed = MultiByteToWideChar(page, flags, bytes.data(), length,
                                     nullptr, 0);
    if (!needed) {
        page = CP_ACP;
        flags = 0;
        needed = MultiByteToWideChar(page, flags, bytes.data(), length,
                                     nullptr, 0);
    }
    if (!needed) return L"Unknown error";
    std::wstring result(static_cast<std::size_t>(needed), L'\0');
    if (MultiByteToWideChar(page, flags, bytes.data(), length,
                            result.data(), needed) != needed)
        return L"Unknown error";
    return result;
}

std::string utf8(std::wstring_view value) {
    if (value.empty()) return {};
    if (value.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("text is too long");
    const int length = static_cast<int>(value.size());
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        value.data(), length, nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::invalid_argument("invalid UTF-16 text");
    std::string result(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, value.data(),
            length, result.data(), bytes, nullptr, nullptr) != bytes)
        throw std::invalid_argument("cannot encode UTF-16 text");
    return result;
}

void json_string(std::string& output, std::wstring_view value) {
    const auto encoded = utf8(value);
    constexpr char hex[] = "0123456789abcdef";
    output.push_back('"');
    for (const auto raw : encoded) {
        const auto c = static_cast<unsigned char>(raw);
        switch (c) {
        case '"': output += "\\\""; break;
        case '\\': output += "\\\\"; break;
        case '\b': output += "\\b"; break;
        case '\f': output += "\\f"; break;
        case '\n': output += "\\n"; break;
        case '\r': output += "\\r"; break;
        case '\t': output += "\\t"; break;
        default:
            if (c < 0x20) {
                output += "\\u00";
                output.push_back(hex[c >> 4]);
                output.push_back(hex[c & 0x0f]);
            } else {
                output.push_back(static_cast<char>(c));
            }
        }
    }
    output.push_back('"');
}

void json_number(std::string& output, std::uint64_t value) {
    output += std::to_string(value);
}

void json_bool(std::string& output, bool value) {
    output += value ? "true" : "false";
}

template<class Number>
void json_optional_number(std::string& output,
                          const std::optional<Number>& value) {
    if (value) json_number(output, *value);
    else output += "null";
}

void json_strings(std::string& output,
                  const std::vector<std::wstring>& values) {
    output.push_back('[');
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i) output.push_back(',');
        json_string(output, values[i]);
    }
    output.push_back(']');
}

std::string inventory_json() {
    const auto disks = gui::enumerate_disks();
    const auto volumes = gui::enumerate_volumes();
    std::string out = "{\"disks\":[";
    for (std::size_t i = 0; i < disks.size(); ++i) {
        if (i) out.push_back(',');
        const auto& disk = disks[i];
        out += "{\"number\":";
        json_number(out, disk.number);
        out += ",\"totalBytes\":";
        json_optional_number(out, disk.total_bytes);
        out += ",\"partitionStyle\":";
        json_number(out, disk.partition_style);
        out += ",\"diskGuid\":";
        json_string(out, disk.disk_guid);
        out += ",\"mbrSignature\":";
        json_optional_number(out, disk.mbr_signature);
        out += ",\"queryError\":";
        json_string(out, disk.query_error);
        out += ",\"partitions\":[";
        for (std::size_t j = 0; j < disk.partitions.size(); ++j) {
            if (j) out.push_back(',');
            const auto& part = disk.partitions[j];
            out += "{\"number\":";
            json_number(out, part.number);
            out += ",\"offsetBytes\":";
            json_number(out, part.offset_bytes);
            out += ",\"lengthBytes\":";
            json_number(out, part.length_bytes);
            out += ",\"style\":";
            json_number(out, part.style);
            out += ",\"partitionGuid\":";
            json_string(out, part.partition_guid);
            out += ",\"type\":";
            json_string(out, part.type);
            out += ",\"name\":";
            json_string(out, part.name);
            out += "}";
        }
        out += "]}";
    }
    out += "],\"volumes\":[";
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        if (i) out.push_back(',');
        const auto& volume = volumes[i];
        out += "{\"guidPath\":";
        json_string(out, volume.guid_path);
        out += ",\"mountPaths\":";
        json_strings(out, volume.mount_paths);
        out += ",\"label\":";
        json_string(out, volume.label);
        out += ",\"filesystem\":";
        json_string(out, volume.filesystem);
        out += ",\"driveType\":";
        json_number(out, volume.drive_type);
        out += ",\"totalBytes\":";
        json_optional_number(out, volume.total_bytes);
        out += ",\"freeBytes\":";
        json_optional_number(out, volume.free_bytes);
        out += ",\"diskNumber\":";
        json_optional_number(out, volume.disk_number);
        out += ",\"partitionNumber\":";
        json_optional_number(out, volume.partition_number);
        out += ",\"extentCount\":";
        json_number(out, volume.extent_count);
        out += ",\"queryError\":";
        json_string(out, volume.query_error);
        out += "}";
    }
    out += "],\"partitionElevated\":";
    json_bool(out, gui::partition_operations_elevated());
    out.push_back('}');
    return out;
}

std::vector<fs::path> path_array(const wchar_t* const* paths,
                                 std::uint32_t count) {
    if (count < 1 || count > 16 || !paths)
        throw std::invalid_argument("member list needs 1 to 16 paths");
    std::vector<fs::path> result;
    result.reserve(count);
    for (std::uint32_t i = 0; i < count; ++i)
        result.emplace_back(checked_text(paths[i], "member path"));
    return result;
}

wchar_t drive_letter(wchar_t value) {
    if (value >= L'a' && value <= L'z') value -= L'a' - L'A';
    if (value < L'A' || value > L'Z')
        throw std::invalid_argument("drive must be an A-Z letter");
    return value;
}

std::optional<poolfs::password::Secret> password_secret(
        const wchar_t* password, std::uint32_t chars) {
    if (!chars) {
        if (password) throw std::invalid_argument("password length is zero");
        return std::nullopt;
    }
    if (!password || chars > 4096)
        throw std::invalid_argument("password pointer or length is invalid");
    for (std::uint32_t i = 0; i < chars; ++i)
        if (!password[i])
            throw std::invalid_argument("password contains a null character");
    const int length = static_cast<int>(chars);
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        password, length, nullptr, 0, nullptr, nullptr);
    if (!bytes) throw std::invalid_argument("password is not valid Unicode");
    std::string converted(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            password, length, converted.data(), bytes, nullptr, nullptr) != bytes) {
        SecureZeroMemory(converted.data(), converted.size());
        throw std::invalid_argument("password conversion failed");
    }
    return poolfs::password::Secret(std::move(converted));
}

int copy_response(char** response, const std::string& body) {
    if (!response) return 1;
    auto* result = static_cast<char*>(CoTaskMemAlloc(body.size() + 1));
    if (!result) return 1;
    std::memcpy(result, body.data(), body.size());
    result[body.size()] = '\0';
    *response = result;
    return 0;
}

template<class Work>
int invoke(char** response, Work&& work) noexcept {
    if (!response) return 1;
    *response = nullptr;
    try {
        return copy_response(response, work());
    } catch (const std::exception& error) {
        try {
            std::string body = "{\"error\":";
            json_string(body, error_wide(error.what()));
            body.push_back('}');
            (void)copy_response(response, body);
        } catch (...) {}
        return 1;
    } catch (...) {
        (void)copy_response(response, "{\"error\":\"Unknown native error\"}");
        return 1;
    }
}

} // namespace

extern "C" int __cdecl poolfs_bridge_inventory(char** response) noexcept {
    return invoke(response, [] { return inventory_json(); });
}

extern "C" int __cdecl poolfs_bridge_scan_images(
        const wchar_t* root_or_null, int recursive, char** response) noexcept {
    return invoke(response, [&] {
        std::vector<fs::path> roots;
        if (root_or_null && *root_or_null)
            roots.emplace_back(checked_text(root_or_null, "scan root"));
        const auto scan = gui::scan_images(roots, recursive != 0);
        std::string out = "{\"images\":[";
        for (std::size_t i = 0; i < scan.images.size(); ++i) {
            if (i) out.push_back(',');
            const auto& image = scan.images[i];
            out += "{\"path\":";
            json_string(out, image.path.wstring());
            out += ",\"bytes\":";
            json_number(out, image.bytes);
            out += ",\"poolId\":";
            json_string(out, image.pool_id);
            out += ",\"memberIndex\":";
            json_number(out, image.member_index);
            out += ",\"memberCount\":";
            json_number(out, image.member_count);
            out += ",\"encrypted\":";
            json_bool(out, image.encrypted);
            out.push_back('}');
        }
        out += "],\"errors\":";
        json_strings(out, scan.errors);
        out += ",\"truncated\":";
        json_bool(out, scan.truncated);
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_load_config(
        const wchar_t* path, char** response) noexcept {
    return invoke(response, [&] {
        const auto config = poolfs::load_mount_config(
            fs::path(checked_text(path, "config path")));
        std::string out = "{\"drive\":";
        json_string(out, std::wstring(1, config.drive) + L":");
        out += ",\"name\":";
        json_string(out, config.name);
        out += ",\"requireEncrypted\":";
        json_bool(out, config.require_encrypted);
        out += ",\"members\":[";
        for (std::size_t i = 0; i < config.members.size(); ++i) {
            if (i) out.push_back(',');
            json_string(out, config.members[i].wstring());
        }
        out += "]}";
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_save_config(
        const wchar_t* path, wchar_t drive,
        const wchar_t* const* members, std::uint32_t count,
        int require_encrypted, char** response) noexcept {
    return invoke(response, [&] {
        const auto destination = fs::path(checked_text(path, "config path"));
        poolfs::MountConfig config;
        config.drive = drive_letter(drive);
        config.members = path_array(members, count);
        config.require_encrypted = require_encrypted != 0;
        if (fs::exists(destination))
            config.name = poolfs::load_mount_config(destination).name;
        gui::save_mount_config(config, destination);
        std::string out = "{\"path\":";
        json_string(out, fs::absolute(destination).lexically_normal().wstring());
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_save_named_config(
        const wchar_t* path, const wchar_t* name, wchar_t drive,
        const wchar_t* const* members, std::uint32_t count,
        int require_encrypted, char** response) noexcept {
    return invoke(response, [&] {
        const auto destination = fs::path(checked_text(path, "config path"));
        poolfs::MountConfig config;
        config.drive = drive_letter(drive);
        config.name = checked_text(name, "pool name");
        config.members = path_array(members, count);
        config.require_encrypted = require_encrypted != 0;
        gui::save_mount_config(config, destination);
        std::string out = "{\"path\":";
        json_string(out, fs::absolute(destination).lexically_normal().wstring());
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_config_is_encrypted(
        const wchar_t* path, char** response) noexcept {
    return invoke(response, [&] {
        const auto config = poolfs::load_mount_config(
            fs::path(checked_text(path, "config path")));
        std::string out = "{\"encrypted\":";
        json_bool(out, gui::config_is_encrypted(config));
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_discover_config(
        const wchar_t* path, char** response) noexcept {
    return invoke(response, [&] {
        const auto report = gui::discover_config(
            fs::path(checked_text(path, "config path")));
        std::string out = "{\"exitCode\":";
        json_number(out, report.exit_code);
        out += ",\"output\":";
        json_string(out, report.output);
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_format_v6(
        const wchar_t* const* paths, const std::uint64_t* sizes_bytes,
        std::uint32_t count, const wchar_t* password,
        std::uint32_t password_chars, char** response) noexcept {
    return invoke(response, [&] {
        if (!sizes_bytes) throw std::invalid_argument("member sizes are missing");
        const auto member_paths = path_array(paths, count);
        std::vector<poolfs::ImageMemberSpec> members;
        members.reserve(count);
        for (std::size_t i = 0; i < member_paths.size(); ++i)
            members.push_back({member_paths[i], sizes_bytes[i]});
        auto secret = password_secret(password, password_chars);
        gui::format_v6_images(members, secret ? &*secret : nullptr);
        std::string out = "{\"membersCreated\":";
        json_number(out, members.size());
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_grow_v6(
        const wchar_t* const* sources, const wchar_t* const* destinations,
        const std::uint64_t* sizes_bytes, std::uint32_t count,
        const wchar_t* password, std::uint32_t password_chars,
        char** response) noexcept {
    return invoke(response, [&] {
        if (!sizes_bytes) throw std::invalid_argument("member sizes are missing");
        const auto old_paths = path_array(sources, count);
        const auto new_paths = path_array(destinations, count);
        std::vector<poolfs::ImageGrowSpec> members;
        members.reserve(count);
        for (std::size_t i = 0; i < old_paths.size(); ++i)
            members.push_back({old_paths[i], new_paths[i], sizes_bytes[i]});
        auto secret = password_secret(password, password_chars);
        poolfs::Pool::GrowV6Images(members,
            secret ? secret->view() : std::string_view{});
        std::string out = "{\"membersCopied\":";
        json_number(out, members.size());
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_mount_config(
        const wchar_t* path, wchar_t expected_drive,
        const wchar_t* const* expected_members, std::uint32_t count,
        int expected_require_encrypted, const wchar_t* password,
        std::uint32_t password_chars, char** response) noexcept {
    return invoke(response, [&] {
        const auto config_path = fs::path(checked_text(path, "config path"));
        poolfs::MountConfig expected;
        expected.drive = drive_letter(expected_drive);
        expected.name = poolfs::load_mount_config(config_path).name;
        expected.members = path_array(expected_members, count);
        expected.require_encrypted = expected_require_encrypted != 0;
        auto secret = password_secret(password, password_chars);
        gui::mount_config(config_path, expected, secret ? &*secret : nullptr);
        std::string out = "{\"drive\":";
        json_string(out, std::wstring(1, expected.drive) + L":");
        out += ",\"mounted\":true}";
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_mount_named_config(
        const wchar_t* path, const wchar_t* expected_name,
        wchar_t expected_drive,
        const wchar_t* const* expected_members, std::uint32_t count,
        int expected_require_encrypted, const wchar_t* password,
        std::uint32_t password_chars, char** response) noexcept {
    return invoke(response, [&] {
        const auto config_path = fs::path(checked_text(path, "config path"));
        poolfs::MountConfig expected;
        expected.drive = drive_letter(expected_drive);
        expected.name = checked_text(expected_name, "pool name");
        expected.members = path_array(expected_members, count);
        expected.require_encrypted = expected_require_encrypted != 0;
        auto secret = password_secret(password, password_chars);
        gui::mount_config(config_path, expected, secret ? &*secret : nullptr);
        std::string out = "{\"drive\":";
        json_string(out, std::wstring(1, expected.drive) + L":");
        out += ",\"mounted\":true}";
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_verify_config(
        const wchar_t* path, const wchar_t* password,
        std::uint32_t password_chars, char** response) noexcept {
    return invoke(response, [&] {
        const auto config = poolfs::load_mount_config(
            fs::path(checked_text(path, "config path")));
        if (gui::mounted(config.drive))
            throw std::runtime_error("unmount the pool before verifying its images");
        auto secret = password_secret(password, password_chars);
        const auto result = gui::verify_pool(config, secret ? &*secret : nullptr);
        std::string out = "{\"capacity\":";
        json_number(out, result.capacity);
        out += ",\"available\":";
        json_number(out, result.available);
        out += ",\"issues\":";
        json_number(out, result.issues);
        out += ",\"incompleteTail\":";
        json_bool(out, result.incomplete_tail);
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_unmount(
        wchar_t drive, char** response) noexcept {
    return invoke(response, [&] {
        const auto letter = drive_letter(drive);
        gui::unmount(letter);
        std::string out = "{\"drive\":";
        json_string(out, std::wstring(1, letter) + L":");
        out += ",\"mounted\":false}";
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_reclaim(
        wchar_t drive, char** response) noexcept {
    return invoke(response, [&] {
        const auto report = gui::reclaim(drive_letter(drive));
        std::string out = "{\"output\":";
        json_string(out, report.output);
        out.push_back('}');
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_mounts(char** response) noexcept {
    return invoke(response, [] {
        std::string out = "{\"mountedDrives\":[";
        bool first = true;
        for (wchar_t drive = L'A'; drive <= L'Z'; ++drive) {
            if (!gui::mounted(drive)) continue;
            if (!first) out.push_back(',');
            json_string(out, std::wstring(1, drive) + L":");
            first = false;
        }
        out += "]}";
        return out;
    });
}

extern "C" int __cdecl poolfs_bridge_partition(
        const PoolFsBridgePartitionRequest* request, int apply,
        char** response) noexcept {
    return invoke(response, [&] {
        if (!request || request->action > 4)
            throw std::invalid_argument("invalid partition action");
        gui::PartitionRequest native;
        native.action = static_cast<gui::PartitionAction>(request->action);
        native.disk_number = request->disk_number;
        native.expected_disk_bytes = request->expected_disk_bytes;
        native.expected_disk_guid = checked_text(request->expected_disk_guid,
            "disk GUID", false);
        native.partition_number = request->partition_number;
        native.expected_partition_guid = checked_text(
            request->expected_partition_guid, "partition GUID", false);
        native.expected_partition_offset = request->expected_partition_offset;
        native.expected_partition_bytes = request->expected_partition_bytes;
        native.size_mib = request->size_mib;
        native.drive_letter = request->drive_letter;
        native.filesystem = checked_text(request->filesystem,
            "file system", false);
        if (native.filesystem.empty()) native.filesystem = L"NTFS";
        native.label = checked_text(request->label, "label", false);
        native.disk_token = checked_text(request->disk_token,
            "disk token", false);
        const auto result = gui::run_partition_operation(native, apply != 0);
        std::string out = "{\"summary\":";
        json_string(out, result.summary);
        out += ",\"diskToken\":";
        json_string(out, result.disk_token);
        out.push_back('}');
        return out;
    });
}

extern "C" void __cdecl poolfs_bridge_free(char* response) noexcept {
    CoTaskMemFree(response);
}
