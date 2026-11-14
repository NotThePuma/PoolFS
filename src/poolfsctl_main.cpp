#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "mount_config.h"
#include "password.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
constexpr std::uint64_t kMinimumMemberBytes = 384ULL * 1024 * 1024;
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

std::optional<Header> inspect_member(const fs::path& path) {
    UniqueHandle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (file.value == INVALID_HANDLE_VALUE) return std::nullopt;
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

void usage() {
    std::wcerr << L"Usage: poolfsctl discover <seed.img> [search-root ...]\n"
               << L"       poolfsctl discover-config <config.json>\n"
               << L"       poolfsctl mount <drive:> <seed.img> [search-root ...]\n"
               << L"       poolfsctl mount-config <config.json>\n"
               << L"       poolfsctl mount --config <config.json>\n"
               << L"       poolfsctl status <drive:>\n"
               << L"       poolfsctl unmount <drive:>\n"
               << L"       poolfsctl reclaim <drive:>\n"
               << L"Positional discovery searches the seed folder and each drive's PoolFS folder.\n"
               << L"Config commands use only the member paths listed in JSON.\n"
               << L"Mount control is limited to this Windows logon session.\n";
}

int run(const std::vector<std::wstring>& args) {
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
