#include "gui_inventory.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winioctl.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace poolfs::gui {
namespace {

namespace fs = std::filesystem;
constexpr std::size_t kHeaderBytes = 4096;
constexpr std::uint64_t kMinimumMemberBytes = 384ULL * 1024 * 1024;
constexpr std::size_t kMaximumIoctlBytes = 1024 * 1024;

class Handle {
public:
    explicit Handle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~Handle() {
        if (value_ != INVALID_HANDLE_VALUE && value_ != nullptr) CloseHandle(value_);
    }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const {
        return value_ != INVALID_HANDLE_VALUE && value_ != nullptr;
    }
private:
    HANDLE value_;
};

std::wstring win_error(DWORD code) {
    wchar_t* buffer = nullptr;
    const DWORD length = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER |
        FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
        nullptr, code, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    if (!length || !buffer) return L"Windows error " + std::to_wstring(code);
    std::wstring message(buffer, length);
    LocalFree(buffer);
    while (!message.empty() &&
           (message.back() == L'\r' || message.back() == L'\n' || message.back() == L' '))
        message.pop_back();
    return message;
}

std::wstring guid_string(const GUID& guid) {
    wchar_t buffer[40]{};
    std::swprintf(buffer, std::size(buffer),
        L"{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
        static_cast<unsigned long>(guid.Data1),
        static_cast<unsigned>(guid.Data2), static_cast<unsigned>(guid.Data3),
        static_cast<unsigned>(guid.Data4[0]), static_cast<unsigned>(guid.Data4[1]),
        static_cast<unsigned>(guid.Data4[2]), static_cast<unsigned>(guid.Data4[3]),
        static_cast<unsigned>(guid.Data4[4]), static_cast<unsigned>(guid.Data4[5]),
        static_cast<unsigned>(guid.Data4[6]), static_cast<unsigned>(guid.Data4[7]));
    return buffer;
}

std::wstring path_key(const fs::path& path) {
    auto native = fs::absolute(path).lexically_normal().wstring();
    std::replace(native.begin(), native.end(), L'/', L'\\');
    std::transform(native.begin(), native.end(), native.begin(), [](wchar_t c) {
        return c >= L'a' && c <= L'z' ? wchar_t(c - (L'a' - L'A')) : c;
    });
    return native;
}

bool resize_needed(DWORD code) {
    return code == ERROR_INSUFFICIENT_BUFFER || code == ERROR_MORE_DATA ||
           code == ERROR_BAD_LENGTH;
}

struct IoctlBuffer {
    std::vector<std::max_align_t> storage;
    std::size_t size = 0;
    const void* data() const { return storage.data(); }
};

IoctlBuffer ioctl_buffer(HANDLE handle, DWORD command,
                         std::size_t initial_bytes, DWORD& returned) {
    for (std::size_t length = initial_bytes; length <= kMaximumIoctlBytes;
         length *= 2) {
        IoctlBuffer bytes;
        bytes.storage.resize((length + sizeof(std::max_align_t) - 1) /
                             sizeof(std::max_align_t));
        returned = 0;
        if (DeviceIoControl(handle, command, nullptr, 0, bytes.storage.data(),
                            static_cast<DWORD>(length), &returned, nullptr)) {
            if (returned > length) return {};
            bytes.size = returned;
            return bytes;
        }
        if (!resize_needed(GetLastError())) return {};
    }
    SetLastError(ERROR_INSUFFICIENT_BUFFER);
    return {};
}

std::vector<std::wstring> volume_mount_paths(const std::wstring& guid_path) {
    for (DWORD length = 256; length <= 65536; length *= 2) {
        std::vector<wchar_t> buffer(length, L'\0');
        DWORD needed = 0;
        if (GetVolumePathNamesForVolumeNameW(guid_path.c_str(), buffer.data(),
                                             length, &needed)) {
            std::vector<std::wstring> paths;
            for (const wchar_t* item = buffer.data(); *item;
                 item += std::wcslen(item) + 1)
                paths.emplace_back(item);
            return paths;
        }
        if (GetLastError() != ERROR_MORE_DATA) return {};
        if (needed > length && needed <= 65536) length = needed - 1;
    }
    return {};
}

VolumeInfo inspect_volume(std::wstring guid_path) {
    VolumeInfo info;
    info.guid_path = std::move(guid_path);
    info.mount_paths = volume_mount_paths(info.guid_path);
    const auto& root = info.mount_paths.empty() ? info.guid_path :
        info.mount_paths.front();
    info.drive_type = GetDriveTypeW(root.c_str());

    std::array<wchar_t, 256> label{};
    std::array<wchar_t, 128> filesystem{};
    if (GetVolumeInformationW(info.guid_path.c_str(), label.data(),
            static_cast<DWORD>(label.size()), nullptr, nullptr, nullptr,
            filesystem.data(), static_cast<DWORD>(filesystem.size()))) {
        info.label = label.data();
        info.filesystem = filesystem.data();
    }
    ULARGE_INTEGER available{}, total{}, free{};
    if (GetDiskFreeSpaceExW(info.guid_path.c_str(), &available, &total, &free)) {
        info.total_bytes = total.QuadPart;
        info.free_bytes = free.QuadPart;
    }

    if (!info.guid_path.ends_with(L'\\')) {
        info.query_error = L"Invalid volume GUID path";
        return info;
    }
    std::wstring device = info.guid_path.substr(0, info.guid_path.size() - 1);
    Handle volume(CreateFileW(device.c_str(), 0,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr));
    if (!volume) {
        info.query_error = win_error(GetLastError());
        return info;
    }
    DWORD returned = 0;
    auto bytes = ioctl_buffer(volume.get(), IOCTL_VOLUME_GET_VOLUME_DISK_EXTENTS,
                              1024, returned);
    if (bytes.size >= sizeof(VOLUME_DISK_EXTENTS)) {
        const auto* extents = reinterpret_cast<const VOLUME_DISK_EXTENTS*>(bytes.data());
        const auto minimum = offsetof(VOLUME_DISK_EXTENTS, Extents) +
            static_cast<std::uint64_t>(extents->NumberOfDiskExtents) *
            sizeof(DISK_EXTENT);
        if (minimum <= bytes.size) {
            info.extent_count = extents->NumberOfDiskExtents;
            if (info.extent_count == 1)
                info.disk_number = extents->Extents[0].DiskNumber;
        }
    }
    PARTITION_INFORMATION_EX partition{};
    if (DeviceIoControl(volume.get(), IOCTL_DISK_GET_PARTITION_INFO_EX,
            nullptr, 0, &partition, sizeof(partition), &returned, nullptr) &&
        returned >= sizeof(partition) && partition.PartitionNumber != 0)
        info.partition_number = partition.PartitionNumber;
    return info;
}

std::vector<std::uint32_t> physical_disk_numbers() {
    std::vector<wchar_t> names(65536);
    for (;;) {
        const DWORD length = QueryDosDeviceW(nullptr, names.data(),
                                             static_cast<DWORD>(names.size()));
        if (length) break;
        const DWORD error = GetLastError();
        if (error != ERROR_INSUFFICIENT_BUFFER || names.size() >= 1024 * 1024)
            throw std::runtime_error("cannot enumerate physical disks");
        names.resize(names.size() * 2);
    }
    std::vector<std::uint32_t> numbers;
    constexpr std::wstring_view prefix = L"PhysicalDrive";
    for (const wchar_t* item = names.data(); *item;
         item += std::wcslen(item) + 1) {
        std::wstring_view name(item);
        if (!name.starts_with(prefix) || name.size() == prefix.size()) continue;
        std::uint64_t number = 0;
        bool digits = true;
        for (const wchar_t c : name.substr(prefix.size())) {
            if (c < L'0' || c > L'9') { digits = false; break; }
            if (number > (std::numeric_limits<std::uint32_t>::max() -
                          static_cast<unsigned>(c - L'0')) / 10) {
                digits = false;
                break;
            }
            number = number * 10 + static_cast<unsigned>(c - L'0');
        }
        if (digits) numbers.push_back(static_cast<std::uint32_t>(number));
    }
    std::sort(numbers.begin(), numbers.end());
    numbers.erase(std::unique(numbers.begin(), numbers.end()), numbers.end());
    return numbers;
}

DiskInfo inspect_disk(std::uint32_t number) {
    DiskInfo info;
    info.number = number;
    const std::wstring path = L"\\\\.\\PhysicalDrive" + std::to_wstring(number);
    HANDLE opened = CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, 0, nullptr);
    if (opened == INVALID_HANDLE_VALUE)
        opened = CreateFileW(path.c_str(), 0,
            FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
            OPEN_EXISTING, 0, nullptr);
    Handle disk(opened);
    if (!disk) {
        info.query_error = win_error(GetLastError());
        return info;
    }
    GET_LENGTH_INFORMATION length{};
    DWORD returned = 0;
    if (DeviceIoControl(disk.get(), IOCTL_DISK_GET_LENGTH_INFO, nullptr, 0,
                        &length, sizeof(length), &returned, nullptr) &&
        returned >= sizeof(length) && length.Length.QuadPart >= 0)
        info.total_bytes = static_cast<std::uint64_t>(length.Length.QuadPart);
    if (!info.total_bytes) {
        DISK_GEOMETRY_EX geometry{};
        if (DeviceIoControl(disk.get(), IOCTL_DISK_GET_DRIVE_GEOMETRY_EX,
                nullptr, 0, &geometry, sizeof(geometry), &returned, nullptr) &&
            returned >= offsetof(DISK_GEOMETRY_EX, Data) &&
            geometry.DiskSize.QuadPart >= 0)
            info.total_bytes = static_cast<std::uint64_t>(geometry.DiskSize.QuadPart);
    }

    auto bytes = ioctl_buffer(disk.get(), IOCTL_DISK_GET_DRIVE_LAYOUT_EX,
                              16 * 1024, returned);
    if (bytes.size < sizeof(DRIVE_LAYOUT_INFORMATION_EX)) {
        info.query_error = win_error(GetLastError());
        return info;
    }
    const auto* layout = reinterpret_cast<const DRIVE_LAYOUT_INFORMATION_EX*>(bytes.data());
    const auto minimum = offsetof(DRIVE_LAYOUT_INFORMATION_EX, PartitionEntry) +
        static_cast<std::uint64_t>(layout->PartitionCount) *
        sizeof(PARTITION_INFORMATION_EX);
    if (minimum > bytes.size) {
        info.query_error = L"Truncated partition layout";
        return info;
    }
    info.partition_style = layout->PartitionStyle;
    const GUID empty_guid{};
    if (layout->PartitionStyle == PARTITION_STYLE_GPT &&
        !IsEqualGUID(layout->Gpt.DiskId, empty_guid))
        info.disk_guid = guid_string(layout->Gpt.DiskId);
    else if (layout->PartitionStyle == PARTITION_STYLE_MBR &&
             layout->Mbr.Signature != 0)
        info.mbr_signature = layout->Mbr.Signature;
    for (DWORD i = 0; i < layout->PartitionCount; ++i) {
        const auto& part = layout->PartitionEntry[i];
        if (part.PartitionNumber == 0 || part.PartitionLength.QuadPart <= 0 ||
            part.StartingOffset.QuadPart < 0) continue;
        PartitionInfo entry;
        entry.number = part.PartitionNumber;
        entry.offset_bytes = static_cast<std::uint64_t>(part.StartingOffset.QuadPart);
        entry.length_bytes = static_cast<std::uint64_t>(part.PartitionLength.QuadPart);
        entry.style = part.PartitionStyle;
        if (part.PartitionStyle == PARTITION_STYLE_GPT) {
            if (!IsEqualGUID(part.Gpt.PartitionId, empty_guid))
                entry.partition_guid = guid_string(part.Gpt.PartitionId);
            entry.type = guid_string(part.Gpt.PartitionType);
            entry.name = part.Gpt.Name;
        } else if (part.PartitionStyle == PARTITION_STYLE_MBR) {
            wchar_t code[8]{};
            std::swprintf(code, std::size(code), L"0x%02X", part.Mbr.PartitionType);
            entry.type = code;
        }
        info.partitions.push_back(std::move(entry));
    }
    return info;
}

std::uint32_t header_number32(const std::array<std::uint8_t, kHeaderBytes>& data,
                              std::size_t offset) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |=
        std::uint32_t(data[offset + i]) << (i * 8);
    return value;
}

std::uint64_t header_number64(const std::array<std::uint8_t, kHeaderBytes>& data,
                              std::size_t offset) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |=
        std::uint64_t(data[offset + i]) << (i * 8);
    return value;
}

std::uint32_t header_fnv(const std::array<std::uint8_t, kHeaderBytes>& data) {
    std::uint32_t value = 2166136261U;
    for (std::size_t i = 0; i < data.size(); ++i) {
        value ^= i >= 56 && i < 60 ? 0 : data[i];
        value *= 16777619U;
    }
    return value;
}

bool parse_header(const std::array<std::uint8_t, kHeaderBytes>& data,
                  std::uint64_t file_bytes, ImageInfo& image) {
    constexpr std::array<std::uint8_t, 8> magic{
        'P', 'O', 'O', 'L', 'F', 'S', '0', '6'};
    if (!std::equal(magic.begin(), magic.end(), data.begin()) ||
        header_number32(data, 8) != 6 ||
        (data[46] != 2 && data[46] != 3) ||
        header_number32(data, 56) != header_fnv(data) ||
        header_number64(data, 48) != file_bytes ||
        file_bytes < kMinimumMemberBytes || file_bytes % kHeaderBytes != 0)
        return false;
    const auto count = data[45];
    const auto index = data[44];
    if (count < 1 || count > 16 || index >= count) return false;
    if (data[46] == 3) {
        const auto iterations = header_number32(data, 544);
        if (iterations < 100000 || iterations > 10000000 ||
            std::all_of(data.begin() + 512, data.begin() + 544,
                [](std::uint8_t byte) { return byte == 0; }))
            return false;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto offset = 64 + i * 24;
        const auto size = header_number64(data, offset + 16);
        if (size < kMinimumMemberBytes || size % kHeaderBytes != 0)
            return false;
        for (std::size_t prior = 0; prior < i; ++prior)
            if (std::equal(data.begin() + offset,
                           data.begin() + offset + 16,
                           data.begin() + 64 + prior * 24))
                return false;
    }
    const auto member_offset = 64 + index * 24;
    if (!std::equal(data.begin() + 28, data.begin() + 44,
                    data.begin() + member_offset) ||
        header_number64(data, member_offset + 16) != file_bytes)
        return false;
    constexpr wchar_t hex[] = L"0123456789abcdef";
    image.pool_id.clear();
    image.pool_id.reserve(32);
    for (std::size_t i = 12; i < 28; ++i) {
        image.pool_id.push_back(hex[data[i] >> 4]);
        image.pool_id.push_back(hex[data[i] & 0x0f]);
    }
    image.member_index = index;
    image.member_count = count;
    image.encrypted = data[46] == 3;
    return true;
}

bool inspect_image(const fs::path& path, ImageInfo& image) {
    std::error_code error;
    const auto file_bytes = fs::file_size(path, error);
    if (error || file_bytes < kMinimumMemberBytes) return false;
    Handle file(CreateFileW(path.c_str(), GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
        OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
    if (!file) return false;
    std::array<std::uint8_t, kHeaderBytes> first{}, second{};
    DWORD read = 0;
    if (!ReadFile(file.get(), first.data(), static_cast<DWORD>(first.size()),
                  &read, nullptr) || read != first.size() ||
        !ReadFile(file.get(), second.data(), static_cast<DWORD>(second.size()),
                  &read, nullptr) || read != second.size()) return false;
    ImageInfo first_info, second_info;
    const bool a = parse_header(first, file_bytes, first_info);
    const bool b = parse_header(second, file_bytes, second_info);
    if (!a && !b) return false;
    if (a && b && first != second) return false;
    image = a ? std::move(first_info) : std::move(second_info);
    image.path = fs::absolute(path).lexically_normal();
    image.bytes = file_bytes;
    return true;
}

} // namespace

std::vector<VolumeInfo> enumerate_volumes() {
    std::array<wchar_t, 1024> path{};
    HANDLE search = FindFirstVolumeW(path.data(), static_cast<DWORD>(path.size()));
    if (search == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot enumerate Windows volumes");
    std::vector<VolumeInfo> result;
    try {
        do {
            result.push_back(inspect_volume(path.data()));
        } while (FindNextVolumeW(search, path.data(), static_cast<DWORD>(path.size())));
        const DWORD error = GetLastError();
        if (error != ERROR_NO_MORE_FILES)
            throw std::runtime_error("volume enumeration stopped early");
    } catch (...) {
        FindVolumeClose(search);
        throw;
    }
    FindVolumeClose(search);
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        return a.guid_path < b.guid_path;
    });
    return result;
}

std::vector<DiskInfo> enumerate_disks() {
    std::vector<DiskInfo> result;
    for (const auto number : physical_disk_numbers())
        result.push_back(inspect_disk(number));
    return result;
}

std::vector<fs::path> default_image_roots(
        const std::vector<VolumeInfo>& volumes) {
    std::vector<fs::path> roots;
    std::set<std::wstring> seen;
    for (const auto& volume : volumes) {
        const auto& paths = volume.mount_paths.empty() ?
            std::vector<std::wstring>{volume.guid_path} : volume.mount_paths;
        for (const auto& mount : paths) {
            fs::path root = fs::path(mount) / L"PoolFS";
            std::error_code error;
            if (fs::is_directory(root, error) && seen.insert(path_key(root)).second)
                roots.push_back(std::move(root));
        }
    }
    return roots;
}

ImageScanResult scan_images(const std::vector<fs::path>& roots, bool recursive,
                            std::size_t max_entries, std::size_t max_depth) {
    ImageScanResult result;
    const auto selected_roots = roots.empty() ?
        default_image_roots(enumerate_volumes()) : roots;
    std::set<std::wstring> seen_roots;
    std::set<std::wstring> seen_images;
    std::size_t visited = 0;
    const auto consume = [&]() {
        if (visited >= max_entries) { result.truncated = true; return false; }
        ++visited;
        return true;
    };
    const auto inspect = [&](const fs::path& path) {
        if (!seen_images.insert(path_key(path)).second) return;
        ImageInfo image;
        if (inspect_image(path, image)) result.images.push_back(std::move(image));
    };
    for (const auto& root : selected_roots) {
        if (result.truncated) break;
        if (!seen_roots.insert(path_key(root)).second) continue;
        std::error_code error;
        if (fs::is_regular_file(root, error)) {
            if (consume()) inspect(root);
            continue;
        }
        if (!fs::is_directory(root, error)) {
            result.errors.push_back(root.wstring() + L": folder is unavailable");
            continue;
        }
        if (recursive) {
            fs::recursive_directory_iterator it(root,
                fs::directory_options::skip_permission_denied, error), end;
            if (error) {
                result.errors.push_back(root.wstring() + L": scan error " +
                                        std::to_wstring(error.value()));
                continue;
            }
            while (it != end && !result.truncated) {
                if (!consume()) break;
                if (static_cast<std::size_t>(it.depth()) >= max_depth)
                    it.disable_recursion_pending();
                if (it->is_regular_file(error) && !error) inspect(it->path());
                error.clear();
                it.increment(error);
                if (error) {
                    result.errors.push_back(root.wstring() + L": scan error " +
                                            std::to_wstring(error.value()));
                    break;
                }
            }
        } else {
            fs::directory_iterator it(root,
                fs::directory_options::skip_permission_denied, error), end;
            if (error) {
                result.errors.push_back(root.wstring() + L": scan error " +
                                        std::to_wstring(error.value()));
                continue;
            }
            while (it != end && !result.truncated) {
                if (!consume()) break;
                if (it->is_regular_file(error) && !error) inspect(it->path());
                error.clear();
                it.increment(error);
                if (error) {
                    result.errors.push_back(root.wstring() + L": scan error " +
                                            std::to_wstring(error.value()));
                    break;
                }
            }
        }
    }
    std::sort(result.images.begin(), result.images.end(),
        [](const auto& a, const auto& b) { return a.path < b.path; });
    return result;
}

} // namespace poolfs::gui
