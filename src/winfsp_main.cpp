#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <intrin.h>
// Zig's MinGW headers and WinFsp both define this name differently.
#ifdef _ReadWriteBarrier
#undef _ReadWriteBarrier
#endif
#include <winfsp/winfsp.h>
#include <sddl.h>
#include <shellapi.h>
#include <shlobj.h>

#include "core.h"
#include "mount_config.h"
#include "password.h"
#include "mount_stats.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace {

bool running_elevated() {
    HANDLE token = nullptr;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    const bool elevated = GetTokenInformation(token, TokenElevation,
        &elevation, sizeof(elevation), &bytes) && elevation.TokenIsElevated;
    CloseHandle(token);
    return elevated;
}

constexpr std::uint64_t kAllocationUnit = 4096;
constexpr int kMaximumRawMembers = 16;
constexpr int kMaximumImageMembers = 16;

struct Handle {
    std::string name;
    std::uint64_t id = 0;
    bool root = false;
    bool deleted = false;
};

struct MountState {
    explicit MountState(const std::filesystem::path& first,
                        const std::filesystem::path& second)
        : pool(first, second) {
        Initialize();
    }

    MountState(const poolfs::RawMemberSpec& first,
               const poolfs::RawMemberSpec& second)
        : pool(first, second) {
        Initialize();
    }

    MountState(poolfs::Pool::RawV3Tag tag,
               const std::vector<poolfs::RawMemberSpec>& members)
        : pool(tag, members) {
        Initialize();
    }

    MountState(poolfs::Pool::V3ImageTag tag,
               const std::vector<std::filesystem::path>& members)
        : pool(tag, members) {
        Initialize();
    }

    MountState(poolfs::Pool::V4ImageTag tag,
               const std::vector<std::filesystem::path>& members)
        : pool(tag, members) {
        Initialize();
    }
    MountState(poolfs::Pool::V5ImageTag tag,
               const std::vector<std::filesystem::path>& members)
        : pool(tag, members) {
        Initialize();
    }
    MountState(poolfs::Pool::V6ImageTag tag,
               const std::vector<std::filesystem::path>& members)
        : pool(tag, members), is_v6(true) {
        Initialize();
    }
    MountState(poolfs::Pool::V6ImageTag tag,
               const std::vector<std::filesystem::path>& members,
               std::string_view password)
        : pool(tag, members, password), is_v6(true) {
        Initialize();
    }

    void Initialize() {
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        timestamp = (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) |
                    now.dwLowDateTime;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"O:SYG:SYD:(A;;FA;;;WD)", SDDL_REVISION_1, &security, nullptr)) {
            throw std::runtime_error("could not create prototype security descriptor");
        }
    }

    ~MountState() {
        if (security) LocalFree(security);
    }

    poolfs::Pool pool;
    poolfs::mount_stats::Publisher stats;
    bool is_v6 = false;
    std::wstring volume_label = L"PoolFS Preview";
    PSECURITY_DESCRIPTOR security = nullptr;
    std::uint64_t timestamp = 0;
    std::mutex operation_mutex;
    std::unordered_set<Handle*> handles;
};

HANDLE stop_event = nullptr;

BOOL WINAPI ConsoleHandler(DWORD event) {
    if (event == CTRL_C_EVENT || event == CTRL_BREAK_EVENT ||
        event == CTRL_CLOSE_EVENT || event == CTRL_SHUTDOWN_EVENT) {
        if (stop_event) SetEvent(stop_event);
        return TRUE;
    }
    return FALSE;
}

MountState& State(FSP_FILE_SYSTEM* fs) {
    return *static_cast<MountState*>(fs->UserContext);
}

NTSTATUS FailureStatus(const std::exception& error) {
    if (const auto* pool_error = dynamic_cast<const poolfs::Error*>(&error)) {
        switch (pool_error->code()) {
        case poolfs::ErrorCode::NotFound:
            return STATUS_OBJECT_NAME_NOT_FOUND;
        case poolfs::ErrorCode::AlreadyExists:
            return STATUS_OBJECT_NAME_COLLISION;
        case poolfs::ErrorCode::NoSpace:
        case poolfs::ErrorCode::JournalFull:
            return STATUS_DISK_FULL;
        case poolfs::ErrorCode::InvalidName:
            return STATUS_OBJECT_NAME_INVALID;
        case poolfs::ErrorCode::InvalidArgument:
            return STATUS_INVALID_PARAMETER;
        case poolfs::ErrorCode::Corrupt:
            return STATUS_FILE_CORRUPT_ERROR;
        case poolfs::ErrorCode::ReadOnly:
            return STATUS_MEDIA_WRITE_PROTECTED;
        case poolfs::ErrorCode::AccessDenied:
            return STATUS_ACCESS_DENIED;
        case poolfs::ErrorCode::Io:
            return STATUS_IO_DEVICE_ERROR;
        }
    }
    if (dynamic_cast<const std::bad_alloc*>(&error))
        return STATUS_INSUFFICIENT_RESOURCES;
    if (dynamic_cast<const std::invalid_argument*>(&error))
        return STATUS_OBJECT_NAME_INVALID;
    return STATUS_UNSUCCESSFUL;
}

template <typename F>
NTSTATUS Guard(F&& operation) noexcept {
    try {
        return operation();
    } catch (const std::exception& error) {
        const NTSTATUS status = FailureStatus(error);
        // Windows routinely probes for absent names and checks for collisions.
        // Those expected results do not need a line of console output each time.
        if (status != STATUS_OBJECT_NAME_NOT_FOUND &&
            status != STATUS_OBJECT_NAME_COLLISION)
            std::cerr << "poolfs: " << error.what() << '\n';
        return status;
    } catch (...) {
        std::cerr << "poolfs: unknown callback failure\n";
        return STATUS_UNSUCCESSFUL;
    }
}

std::string ToUtf8(std::wstring_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("file name too long");
    int length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                                    text.data(), static_cast<int>(text.size()),
                                    nullptr, 0, nullptr, nullptr);
    if (length == 0) throw std::invalid_argument("invalid UTF-16 file name");
    std::string result(static_cast<std::size_t>(length), '\0');
    if (!WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
                             text.data(), static_cast<int>(text.size()),
                             result.data(), length, nullptr, nullptr))
        throw std::invalid_argument("invalid UTF-16 file name");
    return result;
}

std::wstring ToWide(std::string_view text) {
    if (text.empty()) return {};
    if (text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::invalid_argument("file name too long");
    int length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                                     text.data(), static_cast<int>(text.size()),
                                     nullptr, 0);
    if (length == 0) throw std::invalid_argument("invalid UTF-8 file name");
    std::wstring result(static_cast<std::size_t>(length), L'\0');
    if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
                              text.data(), static_cast<int>(text.size()),
                              result.data(), length))
        throw std::invalid_argument("invalid UTF-8 file name");
    return result;
}

bool ValidWindowsComponent(std::wstring_view name) {
    if (name.empty() || name == L"." || name == L".." ||
        name.back() == L'.' || name.back() == L' ') return false;
    for (wchar_t c : name)
        if (c < 32 || c == L'<' || c == L'>' || c == L':' || c == L'"' ||
            c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*')
            return false;
    const auto dot = name.find(L'.');
    std::wstring base(name.substr(0, dot));
    for (auto& c : base) c = static_cast<wchar_t>(std::towupper(c));
    if (base == L"CON" || base == L"PRN" || base == L"AUX" || base == L"NUL")
        return false;
    if (base.size() == 4 &&
        (base.substr(0, 3) == L"COM" || base.substr(0, 3) == L"LPT") &&
        ((base[3] >= L'1' && base[3] <= L'9') ||
         base[3] == L'\u00B9' || base[3] == L'\u00B2' || base[3] == L'\u00B3'))
        return false;
    return true;
}

NTSTATUS ParsePath(const wchar_t* path, bool& root, std::string& leaf) {
    if (!path || path[0] != L'\\') return STATUS_OBJECT_PATH_SYNTAX_BAD;
    if (path[1] == L'\0') {
        root = true;
        leaf.clear();
        return STATUS_SUCCESS;
    }
    root = false;
    leaf.clear();
    const std::wstring_view full(path + 1);
    std::size_t start = 0;
    while (start < full.size()) {
        const auto end = full.find(L'\\', start);
        const auto stop = end == std::wstring_view::npos ? full.size() : end;
        const auto component = full.substr(start, stop - start);
        if (!ValidWindowsComponent(component)) return STATUS_OBJECT_NAME_INVALID;
        if (!leaf.empty()) leaf.push_back('/');
        leaf += ToUtf8(component);
        if (leaf.size() > 1024) return STATUS_OBJECT_NAME_INVALID;
        if (end == std::wstring_view::npos) break;
        start = end + 1;
        if (start == full.size()) return STATUS_OBJECT_NAME_INVALID;
    }
    return STATUS_SUCCESS;
}

std::string HandleName(MountState& state, const Handle& handle) {
    return state.is_v6 ? state.pool.NameForId(handle.id) : handle.name;
}
bool HandleDirectory(MountState& state, const Handle& handle) {
    return handle.root || (state.is_v6 &&
        state.pool.Stat(HandleName(state, handle)).is_directory);
}

std::uint64_t AllocationSize(std::uint64_t file_size) {
    if (file_size > std::numeric_limits<std::uint64_t>::max() - (kAllocationUnit - 1))
        return file_size;
    return ((file_size + kAllocationUnit - 1) / kAllocationUnit) * kAllocationUnit;
}

void FillInfo(const MountState& state, bool root, std::uint64_t size,
              FSP_FSCTL_FILE_INFO* info) {
    *info = {};
    info->FileAttributes = root ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
    info->FileSize = root ? 0 : size;
    info->AllocationSize = root ? 0 : AllocationSize(size);
    info->CreationTime = state.timestamp;
    info->LastAccessTime = state.timestamp;
    info->LastWriteTime = state.timestamp;
    info->ChangeTime = state.timestamp;
}

void FillInfo(const poolfs::Entry& entry, FSP_FSCTL_FILE_INFO* info) {
    *info = {};
    info->FileAttributes = entry.attributes;
    info->FileSize = entry.is_directory ? 0 : entry.size;
    info->AllocationSize = entry.is_directory ? 0 : AllocationSize(entry.size);
    info->CreationTime = entry.creation_time;
    info->LastAccessTime = entry.last_access_time;
    info->LastWriteTime = entry.last_write_time;
    info->ChangeTime = entry.change_time;
    info->IndexNumber = entry.id;
}

NTSTATUS FillHandleInfo(MountState& state, const Handle& handle,
                        FSP_FSCTL_FILE_INFO* info) {
    if (handle.deleted) return STATUS_DELETE_PENDING;
    if (state.is_v6) {
        FillInfo(handle.root ? state.pool.Root() :
            state.pool.Stat(HandleName(state, handle)), info);
        return STATUS_SUCCESS;
    }
    if (handle.root) {
        FillInfo(state, true, 0, info);
        return STATUS_SUCCESS;
    }
    FillInfo(state, false, state.pool.Stat(handle.name).size, info);
    return STATUS_SUCCESS;
}

NTSTATUS CopySecurity(MountState& state, PSECURITY_DESCRIPTOR destination,
                      SIZE_T* destination_size) {
    if (!destination_size) return STATUS_SUCCESS;
    const SIZE_T required = GetSecurityDescriptorLength(state.security);
    const SIZE_T available = *destination_size;
    *destination_size = required;
    if (available < required) return STATUS_BUFFER_OVERFLOW;
    if (destination) std::memcpy(destination, state.security, required);
    return STATUS_SUCCESS;
}

NTSTATUS GetVolumeInfo(FSP_FILE_SYSTEM* fs, FSP_FSCTL_VOLUME_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        *info = {};
        info->TotalSize = state.pool.Capacity();
        info->FreeSize = state.pool.Available();
        if (state.volume_label.size() > std::size(info->VolumeLabel))
            return STATUS_INVALID_PARAMETER;
        info->VolumeLabelLength = static_cast<UINT16>(
            state.volume_label.size() * sizeof(wchar_t));
        std::memcpy(info->VolumeLabel, state.volume_label.data(),
                    info->VolumeLabelLength);
        return STATUS_SUCCESS;
    });
}

NTSTATUS GetSecurityByName(FSP_FILE_SYSTEM* fs, PWSTR path,
                           PUINT32 attributes, PSECURITY_DESCRIPTOR descriptor,
                           SIZE_T* descriptor_size) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        bool root;
        std::string leaf;
        NTSTATUS status = ParsePath(path, root, leaf);
        if (status != STATUS_SUCCESS) return status;
        if (state.is_v6) {
            const auto entry = root ? state.pool.Root() : state.pool.Stat(leaf);
            if (attributes) *attributes = entry.attributes;
        } else {
            if (!root) state.pool.Stat(leaf);
            if (attributes) *attributes = root ? FILE_ATTRIBUTE_DIRECTORY : FILE_ATTRIBUTE_NORMAL;
        }
        return CopySecurity(state, descriptor, descriptor_size);
    });
}

NTSTATUS GetSecurity(FSP_FILE_SYSTEM* fs, PVOID,
                     PSECURITY_DESCRIPTOR descriptor, SIZE_T* descriptor_size) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        return CopySecurity(state, descriptor, descriptor_size);
    });
}

NTSTATUS Create(FSP_FILE_SYSTEM* fs, PWSTR path, UINT32 options, UINT32,
                UINT32 attributes, PSECURITY_DESCRIPTOR, UINT64,
                PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        const bool directory = (options & FILE_DIRECTORY_FILE) != 0;
        if (directory && !state.is_v6) return STATUS_NOT_SUPPORTED;
        bool root;
        std::string leaf;
        NTSTATUS status = ParsePath(path, root, leaf);
        if (status != STATUS_SUCCESS) return status;
        if (root) return STATUS_OBJECT_NAME_COLLISION;
        if ((options & FILE_DELETE_ON_CLOSE) && !directory &&
            (attributes & FILE_ATTRIBUTE_READONLY))
            return STATUS_CANNOT_DELETE;
        if (directory) state.pool.MakeDirectory(leaf, attributes);
        else state.pool.Create(leaf, attributes);
        const auto entry = state.pool.Stat(leaf);
        auto handle = std::make_unique<Handle>();
        handle->name = leaf;
        handle->id = entry.id;
        if (state.is_v6) FillInfo(entry, info);
        else FillInfo(state, false, 0, info);
        *context = handle.get();
        state.handles.insert(handle.release());
        return STATUS_SUCCESS;
    });
}

NTSTATUS Open(FSP_FILE_SYSTEM* fs, PWSTR path, UINT32 options, UINT32,
              PVOID* context, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        bool root;
        std::string leaf;
        NTSTATUS status = ParsePath(path, root, leaf);
        if (status != STATUS_SUCCESS) return status;
        if (root && (options & FILE_NON_DIRECTORY_FILE)) return STATUS_FILE_IS_A_DIRECTORY;
        poolfs::Entry entry;
        if (!root) {
            entry = state.pool.Stat(leaf);
            if (entry.is_directory && (options & FILE_NON_DIRECTORY_FILE))
                return STATUS_FILE_IS_A_DIRECTORY;
            if (!entry.is_directory && (options & FILE_DIRECTORY_FILE))
                return STATUS_NOT_A_DIRECTORY;
            if (options & FILE_DELETE_ON_CLOSE) {
                if (!entry.is_directory &&
                    (entry.attributes & FILE_ATTRIBUTE_READONLY))
                    return STATUS_CANNOT_DELETE;
                if (entry.is_directory && !state.pool.IsDirectoryEmpty(leaf))
                    return STATUS_DIRECTORY_NOT_EMPTY;
            }
        }
        if (root && (options & FILE_DELETE_ON_CLOSE)) return STATUS_CANNOT_DELETE;
        auto handle = std::make_unique<Handle>();
        handle->root = root;
        handle->name = leaf;
        handle->id = root && state.is_v6 ? 1 : entry.id;
        status = FillHandleInfo(state, *handle, info);
        if (status != STATUS_SUCCESS) return status;
        *context = handle.get();
        state.handles.insert(handle.release());
        return STATUS_SUCCESS;
    });
}

NTSTATUS Overwrite(FSP_FILE_SYSTEM* fs, PVOID context, UINT32 attributes,
                   BOOLEAN replace_attributes,
                   UINT64, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (HandleDirectory(state, handle)) return STATUS_FILE_IS_A_DIRECTORY;
        const auto name = HandleName(state, handle);
        if (state.is_v6) state.pool.Overwrite(name, attributes, replace_attributes);
        else state.pool.Truncate(name, 0);
        return FillHandleInfo(state, handle, info);
    });
}

VOID Cleanup(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR, ULONG flags) {
    if (!(flags & FspCleanupDelete)) return;
    (void)Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_SUCCESS;
        if (handle.root) return STATUS_CANNOT_DELETE;
        state.pool.Remove(HandleName(state, handle));
        handle.deleted = true;
        return STATUS_SUCCESS;
    });
}

VOID Close(FSP_FILE_SYSTEM* fs, PVOID context) {
    (void)Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        auto* handle = static_cast<Handle*>(context);
        state.handles.erase(handle);
        delete handle;
        return STATUS_SUCCESS;
    });
}

NTSTATUS Read(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer,
              UINT64 offset, ULONG length, PULONG transferred) {
    return Guard([&]() -> NTSTATUS {
        *transferred = 0;
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (HandleDirectory(state, handle)) return STATUS_FILE_IS_A_DIRECTORY;
        if (length == 0) return STATUS_SUCCESS;
        const auto name = HandleName(state, handle);
        const auto size = state.pool.Stat(name).size;
        if (offset >= size) return STATUS_END_OF_FILE;
        auto bytes = std::span<std::byte>(static_cast<std::byte*>(buffer), length);
        *transferred = static_cast<ULONG>(state.pool.Read(name, offset, bytes));
        state.stats.RecordRead(*transferred);
        return STATUS_SUCCESS;
    });
}

NTSTATUS Write(FSP_FILE_SYSTEM* fs, PVOID context, PVOID buffer,
               UINT64 offset, ULONG length, BOOLEAN to_end, BOOLEAN constrained,
               PULONG transferred, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        *transferred = 0;
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (HandleDirectory(state, handle)) return STATUS_FILE_IS_A_DIRECTORY;
        const auto name = HandleName(state, handle);
        const auto current_size = state.pool.Stat(name).size;
        if (to_end) offset = current_size;
        if (constrained) {
            if (offset >= current_size) length = 0;
            else length = static_cast<ULONG>(std::min<std::uint64_t>(length, current_size - offset));
        }
        if (length) {
            auto bytes = std::span<const std::byte>(static_cast<const std::byte*>(buffer), length);
            *transferred = static_cast<ULONG>(state.pool.Write(name, offset, bytes));
            state.stats.RecordWrite(*transferred);
        }
        return FillHandleInfo(state, handle, info);
    });
}

NTSTATUS Flush(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        state.pool.Flush();
        if (context && info) return FillHandleInfo(state, *static_cast<Handle*>(context), info);
        return STATUS_SUCCESS;
    });
}

NTSTATUS GetFileInfo(FSP_FILE_SYSTEM* fs, PVOID context, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        return FillHandleInfo(state, *static_cast<Handle*>(context), info);
    });
}

NTSTATUS SetBasicInfo(FSP_FILE_SYSTEM* fs, PVOID context, UINT32 attributes,
                      UINT64 creation, UINT64 access, UINT64 write, UINT64 change,
                      FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        if (!state.is_v6) return STATUS_NOT_SUPPORTED;
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        state.pool.SetBasicInfo(handle.id, attributes, creation, access, write, change);
        return FillHandleInfo(state, handle, info);
    });
}

NTSTATUS SetFileSize(FSP_FILE_SYSTEM* fs, PVOID context, UINT64 size,
                     BOOLEAN allocation_size, FSP_FSCTL_FILE_INFO* info) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (HandleDirectory(state, handle)) return STATUS_FILE_IS_A_DIRECTORY;
        const auto name = HandleName(state, handle);
        const auto current_size = state.pool.Stat(name).size;
        if (!allocation_size || size < current_size)
            state.pool.Truncate(name, size);
        return FillHandleInfo(state, handle, info);
    });
}

NTSTATUS CanDelete(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (handle.root) return STATUS_CANNOT_DELETE;
        const auto name = HandleName(state, handle);
        const auto entry = state.pool.Stat(name);
        if (!entry.is_directory && (entry.attributes & FILE_ATTRIBUTE_READONLY))
            return STATUS_ACCESS_DENIED;
        if (entry.is_directory && !state.pool.IsDirectoryEmpty(name))
            return STATUS_DIRECTORY_NOT_EMPTY;
        return STATUS_SUCCESS;
    });
}

NTSTATUS Rename(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR,
                PWSTR new_path, BOOLEAN replace_existing) {
    return Guard([&]() -> NTSTATUS {
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        if (replace_existing && !state.is_v6) return STATUS_NOT_SUPPORTED;
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (handle.root) return STATUS_CANNOT_DELETE;
        bool root;
        std::string new_name;
        NTSTATUS status = ParsePath(new_path, root, new_name);
        if (status != STATUS_SUCCESS) return status;
        if (root) return STATUS_OBJECT_NAME_COLLISION;
        const std::string old_name = HandleName(state, handle);
        if (replace_existing && state.is_v6 && old_name != new_name) {
            // WinFsp normally rejects an occupied destination before posting
            // Rename. Keep the same rule if a future caller bypasses that
            // preflight, so an open target never loses its stable file ID.
            try {
                const auto target = state.pool.Stat(new_name);
                for (const Handle* open : state.handles)
                    if (!open->root && !open->deleted && open->id == target.id)
                        return STATUS_ACCESS_DENIED;
            } catch (const poolfs::Error& error) {
                if (error.code() != poolfs::ErrorCode::NotFound) throw;
            }
        }
        state.pool.Rename(old_name, new_name, replace_existing);
        if (!state.is_v6)
            for (Handle* open : state.handles)
                if (!open->root && open->name == old_name) open->name = new_name;
        return STATUS_SUCCESS;
    });
}

NTSTATUS ReadDirectory(FSP_FILE_SYSTEM* fs, PVOID context, PWSTR,
                       PWSTR marker, PVOID buffer, ULONG length,
                       PULONG transferred) {
    return Guard([&]() -> NTSTATUS {
        *transferred = 0;
        auto& state = State(fs);
        std::lock_guard lock(state.operation_mutex);
        const auto& handle = *static_cast<Handle*>(context);
        if (handle.deleted) return STATUS_DELETE_PENDING;
        if (!HandleDirectory(state, handle)) return STATUS_NOT_A_DIRECTORY;
        const std::string marker_name = marker ? ToUtf8(marker) : std::string();
        bool complete = true;
        const auto entries = state.is_v6 ?
            state.pool.ListDirectory(handle.root ? std::string() : HandleName(state, handle)) :
            state.pool.List();
        for (const auto& entry : entries) {
            if (marker && entry.name <= marker_name) continue;
            const std::wstring wide = ToWide(entry.name);
            alignas(FSP_FSCTL_DIR_INFO) std::array<std::byte, 4096> storage{};
            const auto used = sizeof(FSP_FSCTL_DIR_INFO) + wide.size() * sizeof(wchar_t);
            if (used > storage.size() || used > std::numeric_limits<UINT16>::max())
                return STATUS_OBJECT_NAME_INVALID;
            auto* dir = reinterpret_cast<FSP_FSCTL_DIR_INFO*>(storage.data());
            dir->Size = static_cast<UINT16>(used);
            if (state.is_v6) FillInfo(entry, &dir->FileInfo);
            else FillInfo(state, false, entry.size, &dir->FileInfo);
            std::memcpy(dir->FileNameBuf, wide.data(), wide.size() * sizeof(wchar_t));
            if (!FspFileSystemAddDirInfo(dir, buffer, length, transferred)) {
                complete = false;
                break;
            }
        }
        if (complete) FspFileSystemAddDirInfo(nullptr, buffer, length, transferred);
        return STATUS_SUCCESS;
    });
}

FSP_FILE_SYSTEM_INTERFACE Interface() {
    FSP_FILE_SYSTEM_INTERFACE api{};
    api.GetVolumeInfo = GetVolumeInfo;
    api.GetSecurityByName = GetSecurityByName;
    api.Create = Create;
    api.Open = Open;
    api.Overwrite = Overwrite;
    api.Cleanup = Cleanup;
    api.Close = Close;
    api.Read = Read;
    api.Write = Write;
    api.Flush = Flush;
    api.GetFileInfo = GetFileInfo;
    api.SetBasicInfo = SetBasicInfo;
    api.SetFileSize = SetFileSize;
    api.CanDelete = CanDelete;
    api.Rename = Rename;
    api.GetSecurity = GetSecurity;
    api.ReadDirectory = ReadDirectory;
    return api;
}

} // namespace

int main() {
    int raw_argc = 0;
    wchar_t** raw_argv = CommandLineToArgvW(GetCommandLineW(), &raw_argc);
    if (!raw_argv) return 1;
    HANDLE inherited_password_pipe = nullptr;
    std::vector<wchar_t*> adjusted;
    if (raw_argc >= 3 && std::wstring_view(raw_argv[1]) == L"--password-handle") {
        wchar_t* end = nullptr;
        errno = 0;
        const auto value = std::wcstoull(raw_argv[2], &end, 10);
        if (errno || !raw_argv[2][0] || *end || !value ||
            value > std::numeric_limits<std::uintptr_t>::max()) {
            std::wcerr << L"Invalid password handoff handle\n";
            LocalFree(raw_argv);
            return 2;
        }
        inherited_password_pipe = reinterpret_cast<HANDLE>(
            static_cast<std::uintptr_t>(value));
        adjusted.push_back(raw_argv[0]);
        for (int i = 3; i < raw_argc; ++i) adjusted.push_back(raw_argv[i]);
    }
    int argc = adjusted.empty() ? raw_argc : static_cast<int>(adjusted.size());
    wchar_t** argv = adjusted.empty() ? raw_argv : adjusted.data();
    const std::wstring_view mode = argc >= 2 ? std::wstring_view(argv[1]) : L"";
    const bool raw_v2 = argc == 7 && std::wstring_view(argv[1]) == L"--raw";
    const bool raw_v3 = argc >= 9 && argc <= 3 + 3 * kMaximumRawMembers &&
        (argc - 3) % 3 == 0 && std::wstring_view(argv[1]) == L"--raw-v3";
    const bool image_v3 = argc >= 5 && argc <= 3 + kMaximumImageMembers &&
        std::wstring_view(argv[1]) == L"--images-v3";
    const bool image_v4 = argc >= 5 && argc <= 3 + kMaximumImageMembers &&
        std::wstring_view(argv[1]) == L"--images-v4";
    const bool image_v5 = argc >= 5 && argc <= 3 + kMaximumImageMembers &&
        std::wstring_view(argv[1]) == L"--images-v5";
    const bool image_v6 = argc >= 4 && argc <= 3 + kMaximumImageMembers &&
        std::wstring_view(argv[1]) == L"--images-v6";
    const bool named_image_v6 = argc >= 5 && argc <= 4 + kMaximumImageMembers &&
        std::wstring_view(argv[1]) == L"--named-images-v6";
    const bool image_config = argc == 3 && mode == L"--config";
    const bool named_mode = mode == L"--raw" || mode == L"--raw-v3" ||
        mode == L"--images-v3" || mode == L"--images-v4" ||
        mode == L"--images-v5" || mode == L"--images-v6" ||
        mode == L"--named-images-v6" || mode == L"--config";
    const bool unknown_flag = mode.size() >= 2 && mode[0] == L'-' && mode[1] == L'-' && !named_mode;
    const bool image_default = !named_mode && !unknown_flag &&
        argc >= 3 && argc <= 2 + kMaximumImageMembers;
    if (!image_default && !raw_v2 && !raw_v3 && !image_v3 && !image_v4 &&
        !image_v5 && !image_v6 && !named_image_v6 && !image_config) {
        std::wcerr << L"Usage: poolfs-mount <drive-letter:> <member.img> [up to 16 members]\n"
                   << L"       poolfs-mount --config <config.json>\n";
        LocalFree(raw_argv);
        if (inherited_password_pipe) CloseHandle(inherited_password_pipe);
        return 2;
    }
    const poolfs::RawMemberSpec raw_first = raw_v2 ? poolfs::RawMemberSpec{argv[2], argv[3], L""} : poolfs::RawMemberSpec{};
    const poolfs::RawMemberSpec raw_second = raw_v2 ? poolfs::RawMemberSpec{argv[4], argv[5], L""} : poolfs::RawMemberSpec{};
    std::vector<poolfs::RawMemberSpec> raw_v3_members;
    if (raw_v3) {
        raw_v3_members.reserve(static_cast<std::size_t>((argc - 3) / 3));
        for (int i = 3; i < argc; i += 3)
            raw_v3_members.push_back(poolfs::RawMemberSpec{argv[i], argv[i + 1], argv[i + 2]});
    }
    std::optional<poolfs::MountConfig> config;
    std::vector<std::filesystem::path> image_members;
    if (image_config) {
        try {
            config = poolfs::load_mount_config(argv[2]);
        } catch (const std::exception& error) {
            LocalFree(raw_argv);
            if (inherited_password_pipe) CloseHandle(inherited_password_pipe);
            std::cerr << "poolfs-mount: " << error.what() << '\n';
            return 1;
        }
        image_members = config->members;
    } else if (image_v3 || image_v4 || image_v5 || image_v6 ||
               named_image_v6 || image_default) {
        const int first_member_arg = image_default ? 2 : named_image_v6 ? 4 : 3;
        image_members.reserve(static_cast<std::size_t>(argc - first_member_arg));
        for (int i = first_member_arg; i < argc; ++i)
            image_members.emplace_back(argv[i]);
    }
    const std::wstring named_label = named_image_v6 ? argv[2] : L"";
    const std::wstring mount_point = image_config ?
        std::wstring(1, config->drive) + L":" :
        std::wstring(argv[raw_v2 ? 6 : named_image_v6 ? 3 :
            ((raw_v3 || image_v3 || image_v4 || image_v5 || image_v6) ? 2 : 1)]);
    LocalFree(raw_argv);
    if (mount_point.size() != 2 || mount_point[1] != L':') {
        std::wcerr << L"Mount point must be a drive letter, such as P:\n";
        if (inherited_password_pipe) CloseHandle(inherited_password_pipe);
        return 2;
    }
    if (inherited_password_pipe && !image_v6 && !named_image_v6 &&
        !image_default && !image_config) {
        std::wcerr << L"Password handoff is only valid for v6 image pools\n";
        CloseHandle(inherited_password_pipe);
        return 2;
    }

    try {
        if (named_image_v6) poolfs::validate_pool_name(named_label);
        std::unique_ptr<MountState> state;
        if (raw_v3)
            state = std::make_unique<MountState>(poolfs::Pool::RawV3Tag{}, raw_v3_members);
        else if (image_v3)
            state = std::make_unique<MountState>(poolfs::Pool::V3ImageTag{}, image_members);
        else if (image_v4)
            state = std::make_unique<MountState>(poolfs::Pool::V4ImageTag{}, image_members);
        else if (image_v5)
            state = std::make_unique<MountState>(poolfs::Pool::V5ImageTag{}, image_members);
        else if (image_v6 || named_image_v6 || image_default || image_config) {
            const bool encrypted = poolfs::Pool::V6Encrypted(image_members.front());
            if (image_config && config->require_encrypted && !encrypted)
                throw std::runtime_error("mount config requires an encrypted pool");
            if (encrypted) {
                const auto secret = inherited_password_pipe ?
                    poolfs::password::read_pipe(inherited_password_pipe) :
                    poolfs::password::prompt(L"PoolFS password: ");
                if (inherited_password_pipe) {
                    CloseHandle(inherited_password_pipe);
                    inherited_password_pipe = nullptr;
                }
                state = std::make_unique<MountState>(poolfs::Pool::V6ImageTag{},
                    image_members, secret.view());
            } else {
                if (inherited_password_pipe)
                    throw std::runtime_error("password handoff supplied for an unencrypted pool");
                state = std::make_unique<MountState>(poolfs::Pool::V6ImageTag{}, image_members);
            }
            if (image_config) state->volume_label = config->name;
            else if (named_image_v6) state->volume_label = named_label;
        }
        else if (raw_v2)
            state = std::make_unique<MountState>(raw_first, raw_second);
        const NTSTATUS load = FspLoad(nullptr);
        if (load != STATUS_SUCCESS)
            throw std::runtime_error("WinFsp DLL or driver is unavailable");

        FSP_FSCTL_VOLUME_PARAMS params{};
        params.Version = sizeof(params);
        params.SectorSize = 4096;
        params.SectorsPerAllocationUnit = 1;
        params.MaxComponentLength = 255;
        params.CaseSensitiveSearch = 1;
        params.CasePreservedNames = 1;
        params.UnicodeOnDisk = 1;
        params.UmFileContextIsUserContext2 = 1;
        wcscpy_s(params.FileSystemName, L"PoolFS");

        const auto api = Interface();
        FSP_FILE_SYSTEM* fs = nullptr;
        wchar_t disk_device[] = L"WinFsp.Disk";
        NTSTATUS status = FspFileSystemCreate(disk_device,
                                               &params, &api, &fs);
        if (status != STATUS_SUCCESS)
            throw std::runtime_error("could not create WinFsp disk filesystem");
        fs->UserContext = state.get();

        // A plain "P:" from an elevated process creates a per-logon DOS
        // device invisible to ordinary apps. Mount Manager creates a global
        // drive for all logon sessions, including image pools in the GUI.
        std::wstring mutable_mount_point =
            (raw_v2 || raw_v3 || running_elevated()) ?
            L"\\\\.\\" + mount_point : mount_point;
        status = FspFileSystemSetMountPoint(fs, mutable_mount_point.data());
        if (status != STATUS_SUCCESS) {
            FspFileSystemDelete(fs);
            throw std::runtime_error("could not set mount point");
        }

        status = FspFileSystemStartDispatcher(fs, 0);
        if (status != STATUS_SUCCESS) {
            FspFileSystemRemoveMountPoint(fs);
            FspFileSystemDelete(fs);
            throw std::runtime_error("could not start WinFsp dispatcher");
        }

        // The manager can request a graceful unmount from this logon session.
        // Keep the event private to the owner, administrators, and SYSTEM.
        std::wstring event_name = L"Local\\PoolFS.Mount.";
        const wchar_t drive = mount_point[0] >= L'a' && mount_point[0] <= L'z' ?
            mount_point[0] - (L'a' - L'A') : mount_point[0];
        event_name.push_back(drive);
        PSECURITY_DESCRIPTOR event_security = nullptr;
        if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(
                L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;OW)",
                SDDL_REVISION_1, &event_security, nullptr)) {
            FspFileSystemStopDispatcher(fs);
            FspFileSystemRemoveMountPoint(fs);
            FspFileSystemDelete(fs);
            throw std::runtime_error("could not create shutdown event security");
        }
        SECURITY_ATTRIBUTES event_attributes{sizeof(SECURITY_ATTRIBUTES), event_security, FALSE};
        stop_event = CreateEventW(&event_attributes, TRUE, FALSE, event_name.c_str());
        const DWORD event_error = GetLastError();
        if (!stop_event) {
            LocalFree(event_security);
            FspFileSystemStopDispatcher(fs);
            FspFileSystemRemoveMountPoint(fs);
            FspFileSystemDelete(fs);
            throw std::runtime_error("could not create shutdown event");
        }
        if (event_error == ERROR_ALREADY_EXISTS) {
            LocalFree(event_security);
            CloseHandle(stop_event);
            stop_event = nullptr;
            FspFileSystemStopDispatcher(fs);
            FspFileSystemRemoveMountPoint(fs);
            FspFileSystemDelete(fs);
            throw std::runtime_error("a managed mount already owns that drive letter");
        }
        const std::wstring reclaim_prefix = L"Local\\PoolFS.Reclaim." +
            std::wstring(1, drive) + L".";
        HANDLE reclaim_request = CreateEventW(&event_attributes, FALSE, FALSE,
            (reclaim_prefix + L"Request").c_str());
        const bool request_exists = GetLastError() == ERROR_ALREADY_EXISTS;
        HANDLE reclaim_success = CreateEventW(&event_attributes, TRUE, FALSE,
            (reclaim_prefix + L"Success").c_str());
        const bool success_exists = GetLastError() == ERROR_ALREADY_EXISTS;
        HANDLE reclaim_failure = CreateEventW(&event_attributes, TRUE, FALSE,
            (reclaim_prefix + L"Failure").c_str());
        const bool failure_exists = GetLastError() == ERROR_ALREADY_EXISTS;
        HANDLE reclaim_mutex = CreateMutexW(&event_attributes, FALSE,
            (reclaim_prefix + L"Mutex").c_str());
        const bool mutex_exists = GetLastError() == ERROR_ALREADY_EXISTS;
        const bool stats_ready = state->stats.Start(drive, &event_attributes);
        LocalFree(event_security);
        if (!reclaim_request || !reclaim_success || !reclaim_failure || !reclaim_mutex ||
            request_exists || success_exists || failure_exists || mutex_exists ||
            !stats_ready) {
            if (reclaim_request) CloseHandle(reclaim_request);
            if (reclaim_success) CloseHandle(reclaim_success);
            if (reclaim_failure) CloseHandle(reclaim_failure);
            if (reclaim_mutex) CloseHandle(reclaim_mutex);
            CloseHandle(stop_event);
            stop_event = nullptr;
            FspFileSystemStopDispatcher(fs);
            FspFileSystemRemoveMountPoint(fs);
            FspFileSystemDelete(fs);
            throw std::runtime_error("could not create PoolFS mount control and statistics objects");
        }
        SetConsoleCtrlHandler(ConsoleHandler, TRUE);
        std::wcout << L"PoolFS mounted at " << mount_point
                   << L". Press Ctrl+C to unmount.\n";
        const HANDLE controls[]{stop_event, reclaim_request};
        for (;;) {
            const DWORD selected = WaitForMultipleObjects(2, controls, FALSE, INFINITE);
            if (selected == WAIT_OBJECT_0) break;
            if (selected != WAIT_OBJECT_0 + 1) break;
            try {
                {
                    std::lock_guard lock(state->operation_mutex);
                    state->pool.Checkpoint();
                }
                const auto root = std::wstring(1, drive) + L":\\";
                SHChangeNotify(SHCNE_UPDATEITEM, SHCNF_PATHW, root.c_str(), nullptr);
                SetEvent(reclaim_success);
            } catch (const std::exception& error) {
                std::cerr << "poolfs-mount: checkpoint: " << error.what() << '\n';
                SetEvent(reclaim_failure);
            }
        }
        SetConsoleCtrlHandler(ConsoleHandler, FALSE);
        FspFileSystemStopDispatcher(fs);
        FspFileSystemRemoveMountPoint(fs);
        FspFileSystemDelete(fs);
        if (stop_event) CloseHandle(stop_event);
        stop_event = nullptr;
        CloseHandle(reclaim_request);
        CloseHandle(reclaim_success);
        CloseHandle(reclaim_failure);
        CloseHandle(reclaim_mutex);
        return 0;
    } catch (const std::exception& error) {
        if (inherited_password_pipe) CloseHandle(inherited_password_pipe);
        std::cerr << "poolfs-mount: " << error.what() << '\n';
        if (const auto* pool_error = dynamic_cast<const poolfs::Error*>(&error);
            pool_error && pool_error->code() == poolfs::ErrorCode::AccessDenied)
            return 5;
        return 1;
    }
}
