#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "benchmark_io.h"
#include "mount_config.h"
#include "mount_stats.h"
#include "password.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr std::uint64_t kMinimumMemberBytes = 384ULL * 1024 * 1024;
constexpr DWORD kBenchmarkChunkBytes = 1024 * 1024;
constexpr unsigned kBenchmarkDefaultMiB = 32;
constexpr unsigned kBenchmarkMaximumMiB = 1024;
constexpr std::size_t kBlockBytes = 4096;
constexpr std::size_t kManifestOffset = 64;
constexpr std::size_t kManifestEntryBytes = 24;
constexpr std::uint8_t kPlainV6LayoutRevision = 2;
constexpr std::uint8_t kEncryptedV6LayoutRevision = 3;
constexpr std::size_t kEncryptionSaltOffset = 512;
constexpr std::size_t kEncryptionSaltBytes = 32;
constexpr std::size_t kEncryptionIterationsOffset = 544;
using Id = std::array<std::uint8_t, 16>;
using Block = std::array<std::uint8_t, kBlockBytes>;

struct ManifestEntry {
    Id id{};
    std::uint64_t bytes = 0;
    bool operator==(const ManifestEntry&) const = default;
};

struct Header {
    Id pool_id{};
    Id member_id{};
    std::uint8_t index = 0;
    bool encrypted = false;
    std::vector<ManifestEntry> manifest;
    bool operator==(const Header&) const = default;
};

struct Discovery {
    Header seed;
    std::vector<std::optional<fs::path>> paths;
};

struct UniqueHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~UniqueHandle() { if (value != INVALID_HANDLE_VALUE && value != nullptr) CloseHandle(value); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
};

[[noreturn]] void windows_error(const char* action) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), action);
}

std::uint32_t number32(const Block& block, std::size_t offset) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= std::uint32_t(block[offset + i]) << (8 * i);
    return value;
}

std::uint64_t number64(const Block& block, std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= std::uint64_t(block[offset + i]) << (8 * i);
    return value;
}

std::uint32_t fnv32(const Block& block) {
    std::uint32_t value = 2166136261U;
    for (std::size_t i = 0; i < block.size(); ++i) {
        value ^= i >= 56 && i < 60 ? 0 : block[i];
        value *= 16777619U;
    }
    return value;
}

std::optional<Header> parse_header(const Block& block, std::uint64_t file_bytes) {
    constexpr std::array<std::uint8_t, 8> magic{'P','O','O','L','F','S','0','6'};
    if (!std::equal(magic.begin(), magic.end(), block.begin()) ||
        number32(block, 8) != 6 ||
        (block[46] != kPlainV6LayoutRevision &&
         block[46] != kEncryptedV6LayoutRevision) ||
        number32(block, 56) != fnv32(block)) return std::nullopt;
    if (block[46] == kEncryptedV6LayoutRevision &&
        (number32(block, kEncryptionIterationsOffset) < 100000 ||
         number32(block, kEncryptionIterationsOffset) > 10000000 ||
         std::all_of(block.begin() + kEncryptionSaltOffset,
             block.begin() + kEncryptionSaltOffset + kEncryptionSaltBytes,
             [](std::uint8_t value) { return value == 0; })))
        return std::nullopt;
    const auto count = block[45];
    const auto index = block[44];
    if (count < 1 || count > 16 || index >= count ||
        file_bytes < kMinimumMemberBytes || file_bytes % kBlockBytes != 0 ||
        number64(block, 48) != file_bytes) return std::nullopt;
    Header header;
    std::copy_n(block.begin() + 12, 16, header.pool_id.begin());
    std::copy_n(block.begin() + 28, 16, header.member_id.begin());
    header.index = index;
    header.encrypted = block[46] == kEncryptedV6LayoutRevision;
    for (std::size_t i = 0; i < count; ++i) {
        const auto base = kManifestOffset + i * kManifestEntryBytes;
        ManifestEntry entry;
        std::copy_n(block.begin() + base, 16, entry.id.begin());
        entry.bytes = number64(block, base + 16);
        if (entry.bytes < kMinimumMemberBytes || entry.bytes % kBlockBytes != 0)
            return std::nullopt;
        for (const auto& previous : header.manifest)
            if (previous.id == entry.id) return std::nullopt;
        header.manifest.push_back(entry);
    }
    if (header.manifest[index].id != header.member_id ||
        header.manifest[index].bytes != file_bytes) return std::nullopt;
    return header;
}

std::optional<Header> inspect_member(const fs::path& path, DWORD* open_error = nullptr) {
    if (open_error) *open_error = ERROR_SUCCESS;
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) {
        if (open_error) *open_error = GetLastError();
        return std::nullopt;
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file.value, &length) || length.QuadPart < 0) return std::nullopt;
    const auto bytes = static_cast<std::uint64_t>(length.QuadPart);
    if (bytes < kMinimumMemberBytes) return std::nullopt;
    Block first{}, second{};
    DWORD received = 0;
    if (!ReadFile(file.value, first.data(), static_cast<DWORD>(first.size()),
                  &received, nullptr) || received != first.size()) return std::nullopt;
    if (!ReadFile(file.value, second.data(), static_cast<DWORD>(second.size()),
                  &received, nullptr) || received != second.size()) return std::nullopt;
    const auto a = parse_header(first, bytes);
    const auto b = parse_header(second, bytes);
    if (a && b && first != second) return std::nullopt;
    if (a) return a;
    return b;
}

fs::path normalized(const fs::path& path) {
    std::error_code error;
    const auto canonical = fs::weakly_canonical(path, error);
    return error ? fs::absolute(path).lexically_normal() : canonical;
}

std::wstring path_key(const fs::path& path) {
    auto key = normalized(path).wstring();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) {
        return c >= L'a' && c <= L'z' ? wchar_t(c - (L'a' - L'A')) : c;
    });
    return key;
}

std::wstring id_hex(const Id& id) {
    std::wostringstream out;
    out << std::hex << std::setfill(L'0');
    for (const auto byte : id) out << std::setw(2) << unsigned(byte);
    return out.str();
}

void consider(const fs::path& path, Discovery& found,
              std::set<std::wstring>& seen) {
    if (!seen.insert(path_key(path)).second) return;
    const auto header = inspect_member(path);
    if (!header || header->pool_id != found.seed.pool_id) return;
    if (header->manifest != found.seed.manifest ||
        header->encrypted != found.seed.encrypted)
        throw std::runtime_error("matching pool ID has a different member manifest");
    const auto index = header->index;
    if (found.paths[index])
        throw std::runtime_error("two paths claim the same pool member identity");
    found.paths[index] = normalized(path);
}

void scan_root(const fs::path& root, Discovery& found,
               std::set<std::wstring>& seen) {
    std::error_code error;
    if (fs::is_regular_file(root, error)) {
        consider(root, found, seen);
        return;
    }
    if (!fs::is_directory(root, error)) return;
    fs::recursive_directory_iterator it(root,
        fs::directory_options::skip_permission_denied, error), end;
    while (it != end) {
        if (error) {
            error.clear();
            it.increment(error);
            continue;
        }
        const auto path = it->path();
        if (it->is_regular_file(error)) {
            const auto size = it->file_size(error);
            if (!error && size >= kMinimumMemberBytes) consider(path, found, seen);
        }
        error.clear();
        it.increment(error);
    }
}

Discovery discover(const fs::path& seed_path,
                   const std::vector<fs::path>& extra_roots) {
    const auto seed_header = inspect_member(seed_path);
    if (!seed_header)
        throw std::runtime_error("seed is not a valid current v6 member image");
    Discovery found{*seed_header, std::vector<std::optional<fs::path>>(
        seed_header->manifest.size())};
    std::set<std::wstring> seen;
    consider(seed_path, found, seen);
    if (found.paths.size() == 1 && found.paths.front()) return found;
    scan_root(normalized(seed_path).parent_path(), found, seen);
    const DWORD drives = GetLogicalDrives();
    for (int i = 0; i < 26; ++i) {
        if (!(drives & (1U << i))) continue;
        std::wstring folder = L"A:\\PoolFS";
        folder[0] = wchar_t(L'A' + i);
        scan_root(folder, found, seen);
    }
    for (const auto& root : extra_roots) scan_root(root, found, seen);
    return found;
}

// A config names the complete set explicitly. Never search for substitutes:
// silently replacing an unavailable configured path would hide a bad move or
// an out-of-date transport manifest.
Discovery discover_exact(const poolfs::MountConfig& config) {
    if (config.members.empty())
        throw std::runtime_error("mount config has no members");
    const auto seed = inspect_member(config.members.front());
    if (!seed)
        throw std::runtime_error("configured member 0 is missing or invalid");
    if (config.require_encrypted && !seed->encrypted)
        throw std::runtime_error("mount config requires an encrypted pool");
    if (config.members.size() != seed->manifest.size())
        throw std::runtime_error("mount config does not list the complete member set");
    Discovery found{*seed, std::vector<std::optional<fs::path>>(seed->manifest.size())};
    std::set<std::wstring> seen_paths;
    for (std::size_t supplied = 0; supplied < config.members.size(); ++supplied) {
        const auto& path = config.members[supplied];
        if (!seen_paths.insert(path_key(path)).second)
            throw std::runtime_error("mount config lists a member path twice");
        const auto header = inspect_member(path);
        if (!header)
            throw std::runtime_error("configured member " + std::to_string(supplied) +
                                     " is missing or invalid");
        if (header->pool_id != seed->pool_id ||
            header->manifest != seed->manifest ||
            header->encrypted != seed->encrypted)
            throw std::runtime_error("configured member " + std::to_string(supplied) +
                                     " does not belong to this pool");
        if (found.paths[header->index])
            throw std::runtime_error("mount config lists the same member identity twice");
        found.paths[header->index] = normalized(path);
    }
    if (!std::all_of(found.paths.begin(), found.paths.end(),
                     [](const auto& path) { return path.has_value(); }))
        throw std::runtime_error("mount config is missing a pool member");
    return found;
}

void print_discovery(const Discovery& found) {
    std::wcout << L"pool: " << id_hex(found.seed.pool_id) << L'\n';
    std::wcout << L"encryption: " <<
        (found.seed.encrypted ? L"password required" : L"none") << L'\n';
    for (std::size_t i = 0; i < found.paths.size(); ++i) {
        std::wcout << L"member[" << i << L"]: ";
        if (found.paths[i]) std::wcout << found.paths[i]->wstring();
        else std::wcout << L"MISSING";
        std::wcout << L" (" << found.seed.manifest[i].bytes / (1024 * 1024)
                   << L" MiB)\n";
    }
}

std::wstring json_quote(std::wstring_view value) {
    std::wostringstream out;
    out << L'"';
    for (wchar_t c : value) {
        if (c == L'"' || c == L'\\') out << L'\\' << c;
        else if (c == L'\n') out << L"\\n";
        else if (c == L'\r') out << L"\\r";
        else if (c == L'\t') out << L"\\t";
        else if (c < 32) out << L'?';
        else out << c;
    }
    return out.str() + L'"';
}

bool active(wchar_t drive);

int health_config(const fs::path& path, bool json) {
    const auto config = poolfs::load_mount_config(path);
    const DWORD occupied_mask = GetLogicalDrives();
    const bool drive_occupied = occupied_mask &&
        (occupied_mask & (DWORD{1} << (config.drive - L'A')));
    const bool poolfs_drive = drive_occupied && active(config.drive);
    const auto live = poolfs_drive ? poolfs::mount_stats::ReadMembers(config.drive)
                                   : std::nullopt;
    std::optional<Header> seed;
    std::vector<std::wstring> reasons;
    std::vector<std::optional<std::uint64_t>> host_free;
    std::set<std::uint8_t> seen_indexes;
    std::set<std::size_t> live_indexes;
    std::size_t locked_count = 0;
    for (const auto& member : config.members) {
        std::wstring reason;
        DWORD open_error = ERROR_SUCCESS;
        const auto header = inspect_member(member, &open_error);
        bool mounted_member = false;
        if (live && open_error == ERROR_SHARING_VIOLATION) {
            const auto identity = poolfs::mount_stats::IdentifyMember(member);
            if (identity) {
                const auto found = std::find(live->identities.begin(), live->identities.end(), *identity);
                if (found != live->identities.end()) {
                    mounted_member = true;
                    if (!live_indexes.insert(found - live->identities.begin()).second)
                        reason = L"duplicate member";
                    else if (config.require_encrypted && !live->encrypted)
                        reason = L"config requires encryption";
                    else
                        reason = L"mounted";
                }
            }
        }
        if (!seed && header) seed = header;
        std::error_code path_error;
        const bool present = fs::exists(member, path_error);
        if (mounted_member) { /* The mount already checked this member. */ }
        else if (open_error == ERROR_SHARING_VIOLATION) {
            ++locked_count;
            reason = L"locked; check after unmount";
        }
        else if (open_error == ERROR_ACCESS_DENIED) reason = L"access denied";
        else if (path_error) reason = L"host path unavailable";
        else if (!present) reason = L"missing";
        else if (!header) reason = L"unreadable or invalid image";
        else if (seed && header->pool_id != seed->pool_id) reason = L"different pool";
        else if (seed && header->manifest != seed->manifest) reason = L"different member manifest";
        else if (!seen_indexes.insert(header->index).second) reason = L"duplicate member";
        else if (seed && header->encrypted != seed->encrypted) reason = L"encryption mismatch";
        else if (config.require_encrypted && !header->encrypted) reason = L"config requires encryption";
        std::optional<std::uint64_t> available;
        ULARGE_INTEGER free{}, total{}, total_free{};
        if (GetDiskFreeSpaceExW(member.root_path().c_str(), &free, &total, &total_free))
            available = free.QuadPart;
        reasons.push_back(reason);
        host_free.push_back(available);
    }
    const bool mounted = live && live_indexes.size() == live->identities.size() &&
        std::all_of(reasons.begin(), reasons.end(), [](const auto& reason) { return reason == L"mounted"; });
    const bool wrong_count = (seed && seed->manifest.size() != config.members.size()) ||
        (!live_indexes.empty() && live->identities.size() != config.members.size());
    std::wstring summary_reason = L"ready";
    if (wrong_count)
        summary_reason = L"config member count differs from image manifest";
    else {
        for (const auto& reason : reasons) {
            if (!reason.empty() && reason != L"mounted") { summary_reason = reason; break; }
        }
    }
    if (mounted) summary_reason = L"mounted";
    else if (poolfs_drive && !live && locked_count == config.members.size())
        summary_reason = L"PoolFS drive is mounted; member headers are locked. Remount with the updated build to see live member status";
    else if (summary_reason == L"ready" && drive_occupied)
        summary_reason = L"drive letter in use";
    const bool healthy = !drive_occupied && summary_reason == L"ready";
    const auto drive_status = mounted ? L"mounted" : poolfs_drive ? L"PoolFS drive in use" :
        drive_occupied ? L"drive letter in use" : L"available";
    std::string verified;
    fs::path marker_path = path;
    marker_path += L".verified";
    std::ifstream marker(marker_path);
    if (marker) std::getline(marker, verified);
    if (verified.size() > 32) verified.clear();
    if (json) {
        std::wostringstream out;
        out << L"{\"name\":" << json_quote(config.name)
                   << L",\"drive\":" << json_quote(std::wstring(1, config.drive) + L":")
                   << L",\"mountable\":" << (healthy ? L"true" : L"false")
                   << L",\"mounted\":" << (mounted ? L"true" : L"false")
                   << L",\"driveStatus\":" << json_quote(drive_status)
                   << L",\"reason\":" << json_quote(summary_reason)
                   << L",\"lastVerifiedUtc\":";
        if (verified.empty()) out << L"null";
        else out << json_quote(std::wstring(verified.begin(), verified.end()));
        out << L",\"members\":[";
        for (std::size_t i = 0; i < config.members.size(); ++i) {
            if (i) out << L',';
            out << L"{\"path\":" << json_quote(config.members[i].wstring())
                       << L",\"host\":" << json_quote(config.members[i].root_name().wstring())
                       << L",\"hostFreeBytes\":";
            if (host_free[i]) out << *host_free[i];
            else out << L"null";
            out << L",\"status\":" << json_quote(reasons[i].empty() ? L"ready" : reasons[i])
                       << L'}';
        }
        out << L"]}";
        const auto wide = out.str();
        const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide.data(),
            static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
        if (bytes <= 0) throw std::runtime_error("cannot encode health report");
        std::string utf8(bytes, '\0');
        if (!WideCharToMultiByte(CP_UTF8, 0, wide.data(),
                static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr))
            throw std::runtime_error("cannot encode health report");
        std::cout << utf8 << '\n';
    } else {
        std::wcout << config.name << L" (" << config.drive << L":): "
                   << (healthy ? L"members ready for mount attempt" : summary_reason) << L'\n';
        std::wcout << L"Last successful verification: "
                   << (verified.empty() ? L"not recorded" : std::wstring(verified.begin(), verified.end())) << L'\n';
        for (std::size_t i = 0; i < config.members.size(); ++i) {
            std::wcout << L"  " << i + 1 << L". " << config.members[i].wstring()
                       << L" | " << (reasons[i].empty() ? L"ready" : reasons[i])
                       << L" | host free ";
            if (host_free[i]) std::wcout << *host_free[i] << L" bytes\n";
            else std::wcout << L"unknown\n";
        }
    }
    return healthy || mounted ? 0 : 2;
}

bool complete(const Discovery& found) {
    return std::all_of(found.paths.begin(), found.paths.end(),
                       [](const auto& path) { return path.has_value(); });
}

wchar_t drive_letter(std::wstring_view text) {
    if (text.size() != 2 || text[1] != L':' ||
        !((text[0] >= L'A' && text[0] <= L'Z') ||
          (text[0] >= L'a' && text[0] <= L'z')))
        throw std::runtime_error("drive must be a letter such as V:");
    return text[0] >= L'a' ? wchar_t(text[0] - (L'a' - L'A')) : text[0];
}

std::wstring event_name(wchar_t drive) {
    return std::wstring(L"Local\\PoolFS.Mount.") + drive;
}

bool active(wchar_t drive) {
    UniqueHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                  event_name(drive).c_str()));
    return event.value != nullptr;
}

void require_regular_move_file(HANDLE file) {
    BY_HANDLE_FILE_INFORMATION info{};
    if (GetFileType(file) != FILE_TYPE_DISK || !GetFileInformationByHandle(file, &info))
        throw std::runtime_error("move requires ordinary member image files");
    if (info.dwFileAttributes & (FILE_ATTRIBUTE_DIRECTORY | FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("move requires member images without file links");
}

bool mark_for_deletion(HANDLE file) {
    FILE_DISPOSITION_INFO disposition{TRUE};
    return SetFileInformationByHandle(file, FileDispositionInfo, &disposition,
                                     sizeof(disposition)) != FALSE;
}

void copy_image(HANDLE source, HANDLE destination) {
    std::vector<std::byte> buffer(4 * 1024 * 1024);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(source, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
            windows_error("cannot read source member image");
        if (!read) break;
        DWORD written = 0;
        if (!WriteFile(destination, buffer.data(), read, &written, nullptr))
            windows_error("cannot write destination member image");
        if (written != read)
            throw std::runtime_error("destination member image received a short write");
    }
    if (!FlushFileBuffers(destination))
        windows_error("cannot flush destination member image");
}

void publish_image(HANDLE file, const fs::path& destination) {
    const auto name = destination.wstring();
    const auto name_bytes = name.size() * sizeof(wchar_t);
    std::vector<std::byte> buffer(sizeof(FILE_RENAME_INFO) + name_bytes);
    auto* info = reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());
    info->ReplaceIfExists = FALSE;
    info->RootDirectory = nullptr;
    info->FileNameLength = static_cast<DWORD>(name_bytes);
    std::memcpy(info->FileName, name.c_str(), name_bytes + sizeof(wchar_t));
    if (!SetFileInformationByHandle(file, FileRenameInfo, info,
                                   static_cast<DWORD>(buffer.size())))
        windows_error("cannot publish destination member image");
}

void require_host_destination(const fs::path& destination) {
    const auto name = destination.filename().wstring();
    if (name.empty() || name.find_first_of(L":*?\"<>|") != std::wstring::npos ||
        name.back() == L'.' || name.back() == L' ')
        throw std::invalid_argument("destination must name a new image file");
    if (!fs::is_directory(destination.parent_path()))
        throw std::runtime_error("destination folder must already exist");
    if (GetFileAttributesW(destination.c_str()) != INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("destination already exists; choose a new filename");
    const auto host = fs::canonical(destination.parent_path());
    std::array<wchar_t, 32768> root{};
    std::array<wchar_t, 64> filesystem{};
    if (GetVolumePathNameW(host.c_str(), root.data(), static_cast<DWORD>(root.size())) &&
        (active(root[0]) || (GetVolumeInformationW(root.data(), nullptr, 0, nullptr, nullptr,
            nullptr, filesystem.data(), static_cast<DWORD>(filesystem.size())) &&
            CompareStringOrdinal(filesystem.data(), -1, L"PoolFS", -1, TRUE) == CSTR_EQUAL)))
        throw std::runtime_error("choose a host filesystem outside mounted PoolFS drives");
}

int move_member(const fs::path& config_path, const fs::path& source_path,
                const fs::path& destination_path) {
    const auto config = poolfs::load_mount_config(config_path);
    const auto member = std::find_if(config.members.begin(), config.members.end(),
        [&](const fs::path& path) { return poolfs::mount_paths_alias(path, source_path); });
    if (member == config.members.end())
        throw std::invalid_argument("source image is not listed in the mount config");
    auto destination = fs::absolute(destination_path).lexically_normal();
    destination.make_preferred();
    require_host_destination(destination);

    // Hold all members against mounting or another relocation until config commit.
    std::array<UniqueHandle, 16> files;
    const auto index = static_cast<std::size_t>(member - config.members.begin());
    for (std::size_t i = 0; i < config.members.size(); ++i) {
        const DWORD access = GENERIC_READ | (i == index ? DELETE : 0);
        files[i].value = CreateFileW(config.members[i].c_str(), access, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_FLAG_OPEN_REPARSE_POINT | FILE_FLAG_SEQUENTIAL_SCAN,
            nullptr);
        if (files[i].value == INVALID_HANDLE_VALUE)
            windows_error("cannot lock pool members for move; unmount the pool and check member access");
        require_regular_move_file(files[i].value);
    }
    (void)discover_exact(config);
    const auto source = files[index].value;
    auto updated = config;
    updated.members[index] = destination;

    // Publish a fully written image before changing the config. The source is
    // left in place until the updated config has been atomically saved.
    UniqueHandle staging;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        const auto temporary = destination.parent_path() /
            (L".poolfs-moving-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
             std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(attempt));
        staging.value = CreateFileW(temporary.c_str(), GENERIC_WRITE | DELETE, FILE_SHARE_READ,
            nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
        if (staging.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS && GetLastError() != ERROR_ALREADY_EXISTS)
            windows_error("cannot create staging image in destination folder");
    }
    if (staging.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot reserve a staging image name");
    try {
        copy_image(source, staging.value);
        publish_image(staging.value, destination);
        if (!FlushFileBuffers(staging.value))
            windows_error("cannot flush published member image");
        (void)discover_exact(updated);
        if (poolfs::load_mount_config(config_path) != config)
            throw std::runtime_error("mount config changed during the move; reload it and try again");
        poolfs::save_mount_config(updated, config_path);
    } catch (...) {
        if (!mark_for_deletion(staging.value))
            std::cerr << "PoolFS could not clean up the destination copy; check the destination folder.\n";
        throw;
    }
    if (!mark_for_deletion(source))
        std::cerr << "Config saved, but Windows could not remove the original image. "
                     "The old path now holds an extra copy.\n";
    std::cout << "Moved member image and saved mount config.\n";
    return 0;
}

std::wstring quote(std::wstring_view input) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const auto c : input) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') output.push_back(L'\\');
        output.push_back(c);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

fs::path sibling_mount_exe() {
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(nullptr, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        throw std::runtime_error("cannot locate poolfsctl executable");
    return fs::path(buffer.data(), buffer.data() + length).parent_path() /
           L"poolfs-mount.exe";
}

class HandleList {
public:
    explicit HandleList(HANDLE allowed) : allowed_(allowed) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) throw std::runtime_error("cannot size mount password handoff");
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes))
            throw std::runtime_error("cannot prepare mount password handoff");
        initialized_ = true;
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                &allowed_, sizeof(allowed_), nullptr, nullptr))
            throw std::runtime_error("cannot restrict mount password handoff");
    }
    ~HandleList() { if (initialized_) DeleteProcThreadAttributeList(list_); }
    HandleList(const HandleList&) = delete;
    HandleList& operator=(const HandleList&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }
private:
    HANDLE allowed_ = INVALID_HANDLE_VALUE;
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool initialized_ = false;
};

void managed_mount(wchar_t drive, const Discovery& found,
                   std::wstring_view name = {}) {
    if (!complete(found)) throw std::runtime_error("member set is incomplete");
    if (!name.empty()) poolfs::validate_pool_name(name);
    if (active(drive)) throw std::runtime_error("drive already has a managed PoolFS mount");
    std::wstring root = L"A:\\";
    root[0] = drive;
    if (GetDriveTypeW(root.c_str()) != DRIVE_NO_ROOT_DIR)
        throw std::runtime_error("drive letter is already occupied");
    const auto executable = sibling_mount_exe();
    if (!fs::is_regular_file(executable))
        throw std::runtime_error("poolfs-mount.exe is missing beside poolfsctl.exe");
    std::wstring command = quote(executable.wstring());
    command += L" ";
    if (!name.empty())
        command += L"--named-images-v6 " + quote(std::wstring(name)) + L" ";
    command += drive;
    command += L":";
    for (const auto& path : found.paths) command += L" " + quote(path->wstring());
    std::optional<poolfs::password::Secret> secret;
    UniqueHandle pipe_read, pipe_write;
    if (found.seed.encrypted) {
        secret.emplace(poolfs::password::prompt(L"PoolFS password: "));
        SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&pipe_read.value, &pipe_write.value, &attributes, 0) ||
            !SetHandleInformation(pipe_write.value, HANDLE_FLAG_INHERIT, 0))
            throw std::runtime_error("cannot create mount password handoff");
        command = quote(executable.wstring()) + L" --password-handle " +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(pipe_read.value)) +
            command.substr(quote(executable.wstring()).size());
    }
    std::vector<wchar_t> mutable_command(command.begin(), command.end());
    mutable_command.push_back(L'\0');
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = found.seed.encrypted ? sizeof(startup) :
        sizeof(startup.StartupInfo);
    std::unique_ptr<HandleList> handles;
    if (found.seed.encrypted) {
        handles = std::make_unique<HandleList>(pipe_read.value);
        startup.lpAttributeList = handles->get();
    }
    PROCESS_INFORMATION process{};
    if (!CreateProcessW(executable.c_str(), mutable_command.data(), nullptr,
                        nullptr, found.seed.encrypted ? TRUE : FALSE,
                        CREATE_NO_WINDOW | (found.seed.encrypted ?
                            EXTENDED_STARTUPINFO_PRESENT : 0), nullptr, nullptr,
                        &startup.StartupInfo, &process))
        throw std::runtime_error("cannot start poolfs-mount.exe");
    UniqueHandle child(process.hProcess), thread(process.hThread);
    if (found.seed.encrypted) {
        CloseHandle(pipe_read.value);
        pipe_read.value = INVALID_HANDLE_VALUE;
        poolfs::password::write_pipe(pipe_write.value, secret->view());
        CloseHandle(pipe_write.value);
        pipe_write.value = INVALID_HANDLE_VALUE;
        secret.reset();
    }
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (active(drive)) {
            std::wcout << L"mounted at " << drive << L": (process "
                       << process.dwProcessId << L")\n";
            return;
        }
        if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            GetExitCodeProcess(child.value, &code);
            if (found.seed.encrypted && code == 5)
                throw std::runtime_error("invalid PoolFS password or encrypted header");
            throw std::runtime_error("poolfs-mount exited before becoming ready (exit " +
                                     std::to_string(code) + ")");
        }
        Sleep(125);
    }
    throw std::runtime_error("poolfs-mount did not become ready within 10 seconds");
}

void managed_unmount(wchar_t drive) {
    UniqueHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                  event_name(drive).c_str()));
    if (event.value == nullptr)
        throw std::runtime_error("no managed PoolFS mount on this drive in this logon session");
    if (!SetEvent(event.value)) throw std::runtime_error("cannot request unmount");
    CloseHandle(event.value);
    event.value = nullptr;
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (!active(drive)) {
            std::wcout << L"unmounted " << drive << L":\n";
            return;
        }
        Sleep(125);
    }
    throw std::runtime_error("unmount did not finish within 10 seconds");
}

void managed_reclaim(wchar_t drive) {
    if (!active(drive))
        throw std::runtime_error("no managed PoolFS mount on this drive");
    const auto prefix = std::wstring(L"Local\\PoolFS.Reclaim.") + drive + L".";
    UniqueHandle request(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                    (prefix + L"Request").c_str()));
    UniqueHandle success(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                    (prefix + L"Success").c_str()));
    UniqueHandle failure(OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE,
                                    (prefix + L"Failure").c_str()));
    UniqueHandle stop(OpenEventW(SYNCHRONIZE, FALSE, event_name(drive).c_str()));
    UniqueHandle mutex(OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE,
                                  (prefix + L"Mutex").c_str()));
    if (!request.value || !success.value || !failure.value || !stop.value ||
        !mutex.value)
        throw std::runtime_error("this mount does not support reclaim; remount with the updated PoolFS");
    const auto root = std::wstring(1, drive) + L":\\";
    ULARGE_INTEGER before{}, total{}, total_free{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &before, &total, &total_free))
        throw std::runtime_error("cannot read mounted free space");
    const DWORD acquired = WaitForSingleObject(mutex.value, 30000);
    if (acquired != WAIT_OBJECT_0 && acquired != WAIT_ABANDONED)
        throw std::runtime_error("timed out waiting for another reclaim operation");
    try {
        if (!ResetEvent(success.value) || !ResetEvent(failure.value) ||
            !SetEvent(request.value))
            throw std::runtime_error("cannot request checkpoint rotation");
        const HANDLE results[]{success.value, failure.value, stop.value};
        const DWORD result = WaitForMultipleObjects(3, results, FALSE, 30000);
        if (result == WAIT_OBJECT_0 + 1)
            throw std::runtime_error("checkpoint rotation failed; see poolfs-mount stderr");
        if (result != WAIT_OBJECT_0)
            throw std::runtime_error("checkpoint rotation did not complete within 30 seconds");
    } catch (...) {
        ReleaseMutex(mutex.value);
        throw;
    }
    ReleaseMutex(mutex.value);
    ULARGE_INTEGER after{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &after, &total, &total_free))
        throw std::runtime_error("checkpoint rotated, but mounted free space cannot be read");
    std::wcout << L"checkpoint rotated on " << drive << L":; available "
               << before.QuadPart << L" -> " << after.QuadPart << L" bytes\n";
}

void print_stats(wchar_t drive, const poolfs::mount_stats::Snapshot& first,
                 const poolfs::mount_stats::Snapshot& latest, bool json) {
    const auto elapsed = std::max<std::uint64_t>(1,
        latest.uptime_ms > first.uptime_ms ? latest.uptime_ms - first.uptime_ms : 1);
    const auto read_delta = latest.read_bytes >= first.read_bytes ?
        latest.read_bytes - first.read_bytes : 0;
    const auto write_delta = latest.write_bytes >= first.write_bytes ?
        latest.write_bytes - first.write_bytes : 0;
    const auto read_rate = static_cast<std::uint64_t>(
        static_cast<double>(read_delta) * 1000.0 / static_cast<double>(elapsed));
    const auto write_rate = static_cast<std::uint64_t>(
        static_cast<double>(write_delta) * 1000.0 / static_cast<double>(elapsed));
    if (json) {
        std::wcout << L"{\"drive\":\"" << drive << L":\",\"readBytesPerSecond\":"
                   << read_rate << L",\"writeBytesPerSecond\":" << write_rate
                   << L",\"readBytes\":" << latest.read_bytes
                   << L",\"writeBytes\":" << latest.write_bytes
                   << L",\"readOperations\":" << latest.read_operations
                   << L",\"writeOperations\":" << latest.write_operations
                   << L",\"uptimeMs\":" << latest.uptime_ms
                   << L",\"capacityBytes\":" << latest.capacity_bytes
                   << L",\"availableBytes\":" << latest.available_bytes << L"}\n";
    } else {
        constexpr double mib = 1024.0 * 1024.0;
        std::wcout << drive << L":  read " << std::fixed << std::setprecision(2)
                   << static_cast<double>(read_rate) / mib << L" MiB/s, write "
                   << static_cast<double>(write_rate) / mib << L" MiB/s\n"
                   << L"  Total read " << latest.read_bytes << L" bytes ("
                   << latest.read_operations << L" operations), written "
                   << latest.write_bytes << L" bytes ("
                   << latest.write_operations << L" operations)\n"
                   << L"  Available " << latest.available_bytes << L" of "
                   << latest.capacity_bytes << L" bytes; uptime "
                   << latest.uptime_ms / 1000 << L" seconds\n";
    }
    std::wcout.flush();
}

void managed_stats(wchar_t drive, DWORD interval_ms, bool watch, bool json) {
    do {
        const auto first = poolfs::mount_stats::Read(drive);
        Sleep(interval_ms);
        const auto latest = poolfs::mount_stats::Read(drive);
        print_stats(drive, first, latest, json);
    } while (watch);
}

void managed_benchmark(wchar_t drive, unsigned size_mib, bool json) {
    namespace io = poolfs::benchmark_io;
    if (!active(drive))
        throw std::runtime_error("benchmark requires a mounted PoolFS drive in this Windows session");
    const auto available = poolfs::mount_stats::Read(drive).available_bytes;
    if (available < std::uint64_t{size_mib} * kBenchmarkChunkBytes)
        throw std::runtime_error("not enough free space for the benchmark file; choose a smaller size");

    const auto path = std::wstring(1, drive) + L":\\poolfs-benchmark-" +
        std::to_wstring(GetCurrentProcessId()) + L"-" + std::to_wstring(GetTickCount64()) + L".tmp";
    auto file = io::open_file(path, GENERIC_READ | GENERIC_WRITE | DELETE, CREATE_NEW,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING | FILE_FLAG_DELETE_ON_CLOSE);
    std::array<wchar_t, 64> filesystem{};
    if (!GetVolumeInformationByHandleW(file.get(), nullptr, 0, nullptr, nullptr,
                                      nullptr, filesystem.data(), filesystem.size()))
        windows_error("cannot identify the benchmark file's volume");
    if (std::wstring_view(filesystem.data()) != L"PoolFS")
        throw std::runtime_error("benchmark drive changed before the test could start");

    io::Buffer write_buffer(kBenchmarkChunkBytes), read_buffer(kBenchmarkChunkBytes);
    auto* pattern = static_cast<unsigned char*>(write_buffer.get());
    std::uint32_t state = 0x675213be;
    for (DWORD i = 0; i < kBenchmarkChunkBytes; ++i) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        pattern[i] = static_cast<unsigned char>(state);
    }

    const io::Clock clock;
    const auto write_start = clock.now();
    for (std::uint32_t block = 0; block < size_mib; ++block) {
        std::memcpy(pattern, &block, sizeof(block));
        io::write_exact(file.get(), pattern, kBenchmarkChunkBytes);
    }
    io::flush(file.get());
    const double write_ms = clock.milliseconds(write_start);
    io::seek(file.get(), 0);
    double read_ms = 0;
    for (std::uint32_t block = 0; block < size_mib; ++block) {
        const auto read_start = clock.now();
        io::read_exact(file.get(), read_buffer.get(), kBenchmarkChunkBytes);
        read_ms += clock.milliseconds(read_start);
        std::memcpy(pattern, &block, sizeof(block));
        if (std::memcmp(read_buffer.get(), pattern, kBenchmarkChunkBytes) != 0)
            throw std::runtime_error("benchmark read-back data did not match the written file");
    }
    file.close();
    if (GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES)
        throw std::runtime_error("benchmark file was not removed on close");
    if (GetLastError() != ERROR_FILE_NOT_FOUND)
        windows_error("cannot confirm that the benchmark file was removed");
    if (write_ms <= 0 || read_ms <= 0)
        throw std::runtime_error("benchmark timer did not advance");

    const double write_rate = size_mib * 1000.0 / write_ms;
    const double read_rate = size_mib * 1000.0 / read_ms;
    std::wcout << std::fixed << std::setprecision(2);
    if (json) {
        std::wcout << L"{\"drive\":\"" << drive << L":\",\"sizeMiB\":" << size_mib
                   << L",\"writeMiBPerSecond\":" << write_rate
                   << L",\"readMiBPerSecond\":" << read_rate << L"}\n";
    } else {
        std::wcout << drive << L":  " << size_mib << L" MiB sequential benchmark\n"
                   << L"  Write " << write_rate << L" MiB/s (includes flush)\n"
                   << L"  Read  " << read_rate << L" MiB/s (verified)\n";
    }
}

unsigned benchmark_size(std::wstring_view value) {
    unsigned size = 0;
    for (const wchar_t digit : value) {
        if (digit < L'0' || digit > L'9') {
            size = 0;
            break;
        }
        size = size * 10 + static_cast<unsigned>(digit - L'0');
        if (size > kBenchmarkMaximumMiB) break;
    }
    if (size == 0 || size > kBenchmarkMaximumMiB)
        throw std::invalid_argument("benchmark size must be 1 to 1024 MiB");
    return size;
}

void usage() {
    std::wcerr << L"Usage: poolfsctl discover <seed.img> [search-root ...]\n"
               << L"       poolfsctl discover-config <config.json>\n"
               << L"       poolfsctl mount <drive:> <seed.img> [search-root ...]\n"
               << L"       poolfsctl mount-config <config.json>\n"
               << L"       poolfsctl mount --config <config.json>\n"
               << L"       poolfsctl status <drive:>\n"
               << L"       poolfsctl unmount <drive:>\n"
               << L"       poolfsctl reclaim <drive:>\n"
               << L"       poolfsctl stats <drive:> [--json] [--watch] [--interval-ms 1000]\n"
               << L"       poolfsctl benchmark <drive:> [--size-mib 32] [--json]\n"
               << L"       poolfsctl health-config <config.json> [--json]\n"
               << L"       poolfsctl move-member <config.json> <source.img> <destination.img>\n"
               << L"Positional discovery searches the seed folder and each drive's PoolFS folder.\n"
               << L"Config commands use only the member paths listed in JSON.\n"
               << L"Mount control is limited to this Windows logon session.\n";
}

int run(const std::vector<std::wstring>& args) {
    if (args.size() >= 3 && args[1] == L"benchmark") {
        const auto drive = drive_letter(args[2]);
        auto size_mib = kBenchmarkDefaultMiB;
        bool json = false;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == L"--json") json = true;
            else if (args[i] == L"--size-mib" && i + 1 < args.size())
                size_mib = benchmark_size(args[++i]);
            else { usage(); return 2; }
        }
        managed_benchmark(drive, size_mib, json);
        return 0;
    }
    if (args.size() == 5 && args[1] == L"move-member")
        return move_member(args[2], args[3], args[4]);
    if ((args.size() == 3 || args.size() == 4) && args[1] == L"health-config" &&
        (args.size() == 3 || args[3] == L"--json"))
        return health_config(args[2], args.size() == 4);
    const bool config_command = args.size() == 3 &&
        (args[1] == L"discover-config" || args[1] == L"mount-config");
    const bool config_alias = args.size() == 4 && args[1] == L"mount" &&
        args[2] == L"--config";
    if (config_command || config_alias) {
        const auto config = poolfs::load_mount_config(args.back());
        const auto found = discover_exact(config);
        print_discovery(found);
        if (args[1] != L"discover-config")
            managed_mount(config.drive, found, config.name);
        return 0;
    }
    if (args.size() == 3 && (args[1] == L"status" || args[1] == L"unmount" ||
                             args[1] == L"reclaim")) {
        const auto drive = drive_letter(args[2]);
        if (args[1] == L"unmount") { managed_unmount(drive); return 0; }
        if (args[1] == L"reclaim") { managed_reclaim(drive); return 0; }
        const bool mounted = active(drive);
        std::wcout << drive << L": " << (mounted ? L"mounted" : L"not managed") << L'\n';
        return mounted ? 0 : 1;
    }
    if (args.size() >= 3 && args[1] == L"stats") {
        const auto drive = drive_letter(args[2]);
        bool json = false;
        bool watch = false;
        DWORD interval_ms = 1000;
        for (std::size_t i = 3; i < args.size(); ++i) {
            if (args[i] == L"--json") json = true;
            else if (args[i] == L"--watch") watch = true;
            else if (args[i] == L"--interval-ms" && i + 1 < args.size()) {
                const auto value = args[++i];
                if (value.empty() || !std::all_of(value.begin(), value.end(),
                        [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
                    throw std::invalid_argument("stats interval must be 100 to 10000 ms");
                const auto parsed = std::stoul(value);
                if (parsed < 100 || parsed > 10000)
                    throw std::invalid_argument("stats interval must be 100 to 10000 ms");
                interval_ms = static_cast<DWORD>(parsed);
            } else {
                usage();
                return 2;
            }
        }
        managed_stats(drive, interval_ms, watch, json);
        return 0;
    }
    if (args.size() >= 3 && args[1] == L"discover") {
        std::vector<fs::path> roots;
        for (std::size_t i = 3; i < args.size(); ++i) roots.emplace_back(args[i]);
        const auto found = discover(args[2], roots);
        print_discovery(found);
        return complete(found) ? 0 : 2;
    }
    if (args.size() >= 4 && args[1] == L"mount") {
        const auto drive = drive_letter(args[2]);
        std::vector<fs::path> roots;
        for (std::size_t i = 4; i < args.size(); ++i) roots.emplace_back(args[i]);
        const auto found = discover(args[3], roots);
        print_discovery(found);
        managed_mount(drive, found);
        return 0;
    }
    usage();
    return 2;
}

} // namespace

int main() {
    int argc = 0;
    wchar_t** raw = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!raw) return 1;
    std::vector<std::wstring> args;
    for (int i = 0; i < argc; ++i) args.emplace_back(raw[i]);
    LocalFree(raw);
    try {
        return run(args);
    } catch (const std::exception& error) {
        std::cerr << "poolfsctl: " << error.what() << '\n';
        return 1;
    }
}
