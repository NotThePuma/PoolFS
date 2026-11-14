#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <winioctl.h>

#include "member_device.h"
#include "core.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <vector>

namespace poolfs {
namespace {

constexpr std::uint64_t kBlock = 4096;
// RAW volume handles can reject FSCTL_ALLOW_EXTENDED_DASD_IO. Keep all PoolFS
// I/O away from the final sectors instead of relying on that control code.
constexpr std::uint64_t kTailGuardBytes = 16ULL * 1024 * 1024;
constexpr DWORD kScratchBytes = 1024 * 1024;
const GUID kBasicData = {0xebd0a0a2, 0xb9e5, 0x4433,
    {0x87, 0xc0, 0x68, 0xb6, 0xb7, 0x26, 0x99, 0xc7}};
const GUID kEfiSystem = {0xc12a7328, 0xf81f, 0x11d2,
    {0xba, 0x4b, 0x00, 0xa0, 0xc9, 0x3e, 0xc9, 0x3b}};

[[noreturn]] void fail_io(const char* operation) {
    const auto code = GetLastError();
    throw Error(ErrorCode::Io, std::string(operation) + " (Windows error " +
        std::to_string(code) + ")");
}

struct WinHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~WinHandle() { if (value != INVALID_HANDLE_VALUE) CloseHandle(value); }
    WinHandle() = default;
    explicit WinHandle(HANDLE h) : value(h) {}
    WinHandle(const WinHandle&) = delete;
    WinHandle& operator=(const WinHandle&) = delete;
    WinHandle(WinHandle&& other) noexcept : value(other.value) {
        other.value = INVALID_HANDLE_VALUE;
    }
    WinHandle& operator=(WinHandle&& other) noexcept {
        if (this != &other) {
            if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
            value = other.value;
            other.value = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
};

int hex_digit(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

std::uint32_t hex_value(const std::wstring& s, std::size_t begin, std::size_t count) {
    std::uint32_t value = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const int digit = hex_digit(s.at(begin + i));
        if (digit < 0) throw Error(ErrorCode::InvalidArgument, "invalid GUID");
        value = (value << 4) | static_cast<std::uint32_t>(digit);
    }
    return value;
}

GUID parse_guid(std::wstring text) {
    if (text.size() == 38 && text.front() == L'{' && text.back() == L'}')
        text = text.substr(1, 36);
    if (text.size() != 36 || text[8] != L'-' || text[13] != L'-' ||
        text[18] != L'-' || text[23] != L'-')
        throw Error(ErrorCode::InvalidArgument, "invalid GUID");
    GUID guid{};
    guid.Data1 = hex_value(text, 0, 8);
    guid.Data2 = static_cast<WORD>(hex_value(text, 9, 4));
    guid.Data3 = static_cast<WORD>(hex_value(text, 14, 4));
    guid.Data4[0] = static_cast<BYTE>(hex_value(text, 19, 2));
    guid.Data4[1] = static_cast<BYTE>(hex_value(text, 21, 2));
    for (int i = 0; i < 6; ++i)
        guid.Data4[i + 2] = static_cast<BYTE>(hex_value(text, 24 + 2 * i, 2));
    return guid;
}

std::wstring guid_text(const GUID& guid) {
    constexpr wchar_t digits[] = L"0123456789ABCDEF";
    std::wstring out(36, L'0');
    out[8] = out[13] = out[18] = out[23] = L'-';
    const auto put = [&](std::size_t at, std::uint32_t value, int count) {
        for (int i = count - 1; i >= 0; --i) {
            out[at + i] = digits[value & 0xfU];
            value >>= 4;
        }
    };
    put(0, guid.Data1, 8);
    put(9, guid.Data2, 4);
    put(14, guid.Data3, 4);
    put(19, guid.Data4[0], 2);
    put(21, guid.Data4[1], 2);
    for (int i = 0; i < 6; ++i) put(24 + 2 * i, guid.Data4[i + 2], 2);
    return out;
}

std::wstring normalize_volume_path(std::wstring path) {
    if (!path.empty() && path.back() == L'\\') path.pop_back();
    const std::wstring prefix = L"\\\\?\\Volume{";
    if (path.size() != prefix.size() + 36 + 1 ||
        path.compare(0, prefix.size(), prefix) != 0 || path.back() != L'}')
        throw Error(ErrorCode::InvalidArgument,
            "raw member must be an exact volume GUID path, not a drive letter or disk path");
    (void)parse_guid(path.substr(prefix.size(), 36));
    return path;
}

struct PhysicalExtent {
    std::uint32_t disk_number = 0;
    std::uint64_t start_bytes = 0;
    std::uint64_t length_bytes = 0;
};

PhysicalExtent volume_extent(HANDLE volume) {
    VOLUME_DISK_EXTENTS extents{};
    DWORD returned = 0;
    if (!DeviceIoControl(volume, IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                         nullptr, 0, &extents, sizeof(extents), &returned, nullptr)) {
        if (GetLastError() == ERROR_MORE_DATA)
            throw Error(ErrorCode::InvalidArgument,
                        "raw member spans multiple physical disks");
        fail_io("cannot inspect volume disk extents");
    }
    if (returned < sizeof(extents) || extents.NumberOfDiskExtents != 1)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member must occupy exactly one physical disk");
    const auto& extent = extents.Extents[0];
    if (extent.StartingOffset.QuadPart < 0 || extent.ExtentLength.QuadPart <= 0)
        throw Error(ErrorCode::Io, "invalid physical volume extent");
    return PhysicalExtent{extent.DiskNumber,
        static_cast<std::uint64_t>(extent.StartingOffset.QuadPart),
        static_cast<std::uint64_t>(extent.ExtentLength.QuadPart)};
}

std::uint32_t volume_disk_number(HANDLE volume) {
    return volume_extent(volume).disk_number;
}

std::uint32_t disk_for_mount_root(const std::wstring& root) {
    std::array<wchar_t, MAX_PATH> volume_name{};
    if (!GetVolumeNameForVolumeMountPointW(root.c_str(), volume_name.data(),
                                          static_cast<DWORD>(volume_name.size())))
        fail_io("cannot resolve protected Windows volume");
    std::wstring path(volume_name.data());
    if (path.empty() || path.back() != L'\\')
        throw Error(ErrorCode::Io, "invalid protected Windows volume path");
    path.pop_back();
    WinHandle handle(CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE)
        fail_io("cannot open protected Windows volume for inspection");
    return volume_disk_number(handle.value);
}

std::uint32_t disk_for_windows_path(const std::wstring& path) {
    std::array<wchar_t, MAX_PATH> root{};
    if (!GetVolumePathNameW(path.c_str(), root.data(),
                            static_cast<DWORD>(root.size())))
        fail_io("cannot locate Windows system volume");
    return disk_for_mount_root(root.data());
}

void reject_windows_disk(std::uint32_t candidate) {
    std::array<wchar_t, MAX_PATH> windows{};
    const UINT length = GetWindowsDirectoryW(windows.data(),
                                             static_cast<UINT>(windows.size()));
    if (!length || length >= windows.size())
        fail_io("cannot locate Windows directory");
    if (candidate == disk_for_windows_path(windows.data()))
        throw Error(ErrorCode::InvalidArgument, "raw member is on the Windows boot disk");

    // Existing page files can live on disks other than the Windows directory.
    // The configured PagingFiles value may use ?:\pagefile.sys, so inspect
    // ExistingPageFiles for the active resolved drive as well.
    constexpr wchar_t key[] =
        L"SYSTEM\\CurrentControlSet\\Control\\Session Manager\\Memory Management";
    const auto read_multi_string = [&key](const wchar_t* name) {
        DWORD bytes = 0;
        LONG result = RegGetValueW(HKEY_LOCAL_MACHINE, key, name,
                                   RRF_RT_REG_MULTI_SZ, nullptr, nullptr, &bytes);
        if (result == ERROR_FILE_NOT_FOUND) return std::vector<std::wstring>{};
        if (result != ERROR_SUCCESS)
            throw Error(ErrorCode::Io, "cannot inspect Windows page-file configuration");
        std::vector<wchar_t> buffer(bytes / sizeof(wchar_t) + 1, L'\0');
        result = RegGetValueW(HKEY_LOCAL_MACHINE, key, name,
                              RRF_RT_REG_MULTI_SZ, nullptr, buffer.data(), &bytes);
        if (result != ERROR_SUCCESS)
            throw Error(ErrorCode::Io, "cannot inspect Windows page-file configuration");
        std::vector<std::wstring> paths;
        for (const wchar_t* item = buffer.data(); *item; item += std::wcslen(item) + 1)
            paths.emplace_back(item);
        return paths;
    };
    const auto active = read_multi_string(L"ExistingPageFiles");
    const auto configured = read_multi_string(L"PagingFiles");
    const auto inspect_path = [candidate](const std::wstring& text, bool allow_wildcard) {
        for (std::size_t i = 0; i + 2 < text.size(); ++i) {
            const wchar_t drive = text[i];
            if (text[i + 1] == L':' && text[i + 2] == L'\\') {
                if (drive == L'?' && allow_wildcard) return;
                if ((drive >= L'A' && drive <= L'Z') ||
                    (drive >= L'a' && drive <= L'z')) {
                    std::wstring root{drive, L':', L'\\'};
                    if (candidate == disk_for_mount_root(root))
                        throw Error(ErrorCode::InvalidArgument,
                                    "raw member is on a page-file disk");
                    return;
                }
            }
        }
        throw Error(ErrorCode::Io, "cannot resolve Windows page-file location");
    };
    for (const auto& path : active) inspect_path(path, false);
    for (const auto& path : configured) inspect_path(path, !active.empty());
}

struct Layout {
    GUID disk_id{};
    bool contains_efi = false;
    struct BasicPartition {
        GUID id{};
        LARGE_INTEGER start{};
        LARGE_INTEGER length{};
    };
    std::vector<BasicPartition> basic_data;
};

Layout inspect_gpt_disk(std::uint32_t disk_number) {
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(disk_number);
    WinHandle handle(CreateFileW(path.c_str(), GENERIC_READ,
                                 FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr));
    if (handle.value == INVALID_HANDLE_VALUE)
        fail_io("cannot open underlying disk for GPT inspection");
    alignas(DRIVE_LAYOUT_INFORMATION_EX) std::array<std::byte, 64 * 1024> buffer{};
    DWORD returned = 0;
    if (!DeviceIoControl(handle.value, IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                         nullptr, 0, buffer.data(), static_cast<DWORD>(buffer.size()),
                         &returned, nullptr))
        fail_io("cannot inspect underlying GPT");
    if (returned < sizeof(DRIVE_LAYOUT_INFORMATION_EX))
        throw Error(ErrorCode::Io, "truncated underlying disk layout");
    const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(buffer.data());
    if (layout->PartitionStyle != PARTITION_STYLE_GPT)
        throw Error(ErrorCode::InvalidArgument, "raw member requires a GPT disk");
    const std::size_t minimum = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
        static_cast<std::size_t>(layout->PartitionCount) * sizeof(PARTITION_INFORMATION_EX);
    if (minimum > returned)
        throw Error(ErrorCode::Io, "truncated underlying disk partition list");
    Layout out{};
    out.disk_id = layout->Gpt.DiskId;
    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const auto& partition = layout->PartitionEntry[i];
        if (partition.PartitionStyle == PARTITION_STYLE_GPT &&
            IsEqualGUID(partition.Gpt.PartitionType, kEfiSystem))
            out.contains_efi = true;
        if (partition.PartitionStyle == PARTITION_STYLE_GPT &&
            IsEqualGUID(partition.Gpt.PartitionType, kBasicData)) {
            out.basic_data.push_back(Layout::BasicPartition{
                partition.Gpt.PartitionId,
                partition.StartingOffset,
                partition.PartitionLength});
        }
    }
    return out;
}

void move_to(HANDLE volume, std::uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(volume, position, nullptr, FILE_BEGIN))
        fail_io("raw member seek failed");
}

} // namespace

struct RawVolume::Impl {
    WinHandle volume;
    RawVolumeInfo info;
    std::byte* scratch = nullptr;
    std::mutex io_mutex;

    ~Impl() {
        if (scratch) VirtualFree(scratch, 0, MEM_RELEASE);
        // Closing volume also releases FSCTL_LOCK_VOLUME.
    }
};

RawVolume::RawVolume(const std::wstring& volume_path,
                     const std::wstring& expected_disk_guid,
                     const std::wstring& expected_partition_guid)
    : impl_(std::make_unique<Impl>()) {
    const auto path = normalize_volume_path(volume_path);
    const GUID expected = parse_guid(expected_disk_guid);
    const bool explicit_partition = !expected_partition_guid.empty();
    const GUID expected_partition = explicit_partition ?
        parse_guid(expected_partition_guid) : GUID{};
    impl_->volume.value = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
        FILE_FLAG_NO_BUFFERING, nullptr);
    if (impl_->volume.value == INVALID_HANDLE_VALUE)
        fail_io("cannot open raw member volume (run elevated and check the volume path)");

    DWORD returned = 0;
    if (!DeviceIoControl(impl_->volume.value, FSCTL_LOCK_VOLUME,
                         nullptr, 0, nullptr, 0, &returned, nullptr))
        fail_io("cannot exclusively lock raw member volume");

    PARTITION_INFORMATION_EX partition{};
    if (!DeviceIoControl(impl_->volume.value, IOCTL_DISK_GET_PARTITION_INFO_EX,
                         nullptr, 0, &partition, sizeof(partition), &returned, nullptr))
        fail_io("cannot inspect raw member partition");
    if (partition.PartitionStyle != PARTITION_STYLE_GPT ||
        !IsEqualGUID(partition.Gpt.PartitionType, kBasicData))
        throw Error(ErrorCode::InvalidArgument,
                    "raw member must be a GPT basic-data partition");
    if (partition.StartingOffset.QuadPart < 0 ||
        static_cast<std::uint64_t>(partition.StartingOffset.QuadPart) % kBlock != 0)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member partition start must be 4096-byte aligned");
    if (partition.PartitionLength.QuadPart <= 0)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member partition length must be positive");

    const PhysicalExtent extent = volume_extent(impl_->volume.value);
    const std::uint32_t disk_number = extent.disk_number;
    if (extent.start_bytes !=
            static_cast<std::uint64_t>(partition.StartingOffset.QuadPart) ||
        extent.length_bytes !=
            static_cast<std::uint64_t>(partition.PartitionLength.QuadPart))
        throw Error(ErrorCode::InvalidArgument,
                    "raw volume extent differs from its GPT partition");
    reject_windows_disk(disk_number);
    const Layout layout = inspect_gpt_disk(disk_number);
    if (layout.contains_efi)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member disk contains an EFI system partition");
    if (!IsEqualGUID(layout.disk_id, expected))
        throw Error(ErrorCode::InvalidArgument,
                    "underlying GPT disk GUID differs from the expected disk");
    std::size_t matching_partitions = 0;
    for (const auto& candidate : layout.basic_data)
        if (IsEqualGUID(candidate.id, partition.Gpt.PartitionId) &&
            candidate.start.QuadPart == partition.StartingOffset.QuadPart &&
            candidate.length.QuadPart == partition.PartitionLength.QuadPart)
            ++matching_partitions;
    if (matching_partitions != 1)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member does not match a unique GPT basic-data partition");
    if (explicit_partition) {
        if (!IsEqualGUID(partition.Gpt.PartitionId, expected_partition))
            throw Error(ErrorCode::InvalidArgument,
                        "raw member GPT partition GUID differs from the expected partition");
    } else if (layout.basic_data.size() != 1) {
        throw Error(ErrorCode::InvalidArgument,
                    "v2 raw member must be the disk's sole basic-data partition");
    }

    GET_LENGTH_INFORMATION length{};
    if (!DeviceIoControl(impl_->volume.value, IOCTL_DISK_GET_LENGTH_INFO,
                         nullptr, 0, &length, sizeof(length), &returned, nullptr))
        fail_io("cannot inspect raw member length");
    if (length.Length.QuadPart != partition.PartitionLength.QuadPart ||
        length.Length.QuadPart <= static_cast<LONGLONG>(kTailGuardBytes) ||
        static_cast<std::uint64_t>(length.Length.QuadPart) % kBlock != 0)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member length must match its GPT partition and leave a 16 MiB tail guard");

    STORAGE_PROPERTY_QUERY query{};
    query.PropertyId = StorageAccessAlignmentProperty;
    query.QueryType = PropertyStandardQuery;
    STORAGE_ACCESS_ALIGNMENT_DESCRIPTOR alignment{};
    if (!DeviceIoControl(impl_->volume.value, IOCTL_STORAGE_QUERY_PROPERTY,
                         &query, sizeof(query), &alignment, sizeof(alignment),
                         &returned, nullptr))
        fail_io("cannot inspect raw member sector alignment");
    const auto logical = alignment.BytesPerLogicalSector;
    const auto physical = alignment.BytesPerPhysicalSector;
    if (!logical || !physical || logical > kBlock || physical > kBlock ||
        kBlock % logical != 0 || kBlock % physical != 0)
        throw Error(ErrorCode::InvalidArgument,
                    "raw member sector size is unsupported by PoolFS v2");

    impl_->scratch = static_cast<std::byte*>(VirtualAlloc(
        nullptr, kScratchBytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!impl_->scratch) fail_io("cannot allocate aligned raw I/O buffer");
    impl_->info = RawVolumeInfo{path, guid_text(layout.disk_id),
        static_cast<std::uint64_t>(length.Length.QuadPart) - kTailGuardBytes, disk_number,
        logical, physical, guid_text(partition.Gpt.PartitionId),
        static_cast<std::uint64_t>(partition.StartingOffset.QuadPart),
        static_cast<std::uint64_t>(partition.PartitionLength.QuadPart)};
}

RawVolume::~RawVolume() = default;
const RawVolumeInfo& RawVolume::info() const noexcept { return impl_->info; }
std::uint64_t RawVolume::size() const { return impl_->info.bytes; }

void RawVolume::read(std::uint64_t offset, std::byte* data, std::size_t length) {
    if (offset > size() || length > size() - offset)
        throw Error(ErrorCode::InvalidArgument, "raw read exceeds member length");
    if (!length) return;
    std::lock_guard lock(impl_->io_mutex);
    while (length) {
        const auto start = offset & ~(kBlock - 1);
        const auto skip = static_cast<std::size_t>(offset - start);
        const auto take = std::min<std::size_t>(length, kScratchBytes - skip);
        const auto transfer = static_cast<DWORD>((skip + take + kBlock - 1) & ~(kBlock - 1));
        move_to(impl_->volume.value, start);
        DWORD actual = 0;
        if (!ReadFile(impl_->volume.value, impl_->scratch, transfer, &actual, nullptr))
            fail_io("raw member read failed");
        if (actual != transfer)
            throw Error(ErrorCode::Io, "short raw member read");
        std::memcpy(data, impl_->scratch + skip, take);
        offset += take;
        data += take;
        length -= take;
    }
}

void RawVolume::write(std::uint64_t offset, const std::byte* data, std::size_t length) {
    if (offset > size() || length > size() - offset)
        throw Error(ErrorCode::InvalidArgument, "raw write exceeds member length");
    if (offset % kBlock != 0 || length % kBlock != 0)
        throw Error(ErrorCode::InvalidArgument,
                    "raw writes require 4096-byte aligned offsets and lengths");
    if (!length) return;
    std::lock_guard lock(impl_->io_mutex);
    while (length) {
        const DWORD transfer = static_cast<DWORD>(
            std::min<std::size_t>(length, kScratchBytes));
        std::memcpy(impl_->scratch, data, transfer);
        move_to(impl_->volume.value, offset);
        DWORD actual = 0;
        if (!WriteFile(impl_->volume.value, impl_->scratch, transfer, &actual, nullptr))
            fail_io("raw member write failed");
        if (actual != transfer)
            throw Error(ErrorCode::Io, "short raw member write");
        offset += transfer;
        data += transfer;
        length -= transfer;
    }
}

void RawVolume::sync() {
    std::lock_guard lock(impl_->io_mutex);
    if (!FlushFileBuffers(impl_->volume.value))
        fail_io("raw member durable flush failed");
}

} // namespace poolfs
