#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace poolfs::gui {

// Inventory is read-only. Missing metadata stays unknown rather than being
// reported as zero; callers should display query_error when useful.
struct VolumeInfo {
    std::wstring guid_path;
    std::vector<std::wstring> mount_paths;
    std::wstring label;
    std::wstring filesystem;
    unsigned drive_type = 0;
    std::optional<std::uint64_t> total_bytes;
    std::optional<std::uint64_t> free_bytes;
    std::optional<std::uint32_t> disk_number;
    std::optional<std::uint32_t> partition_number;
    std::uint32_t extent_count = 0;
    std::wstring query_error;
};

struct PartitionInfo {
    std::uint32_t number = 0;
    std::uint64_t offset_bytes = 0;
    std::uint64_t length_bytes = 0;
    unsigned style = 0;
    std::wstring partition_guid; // Empty unless a nonzero GPT partition ID is present.
    std::wstring type;
    std::wstring name;
};

struct DiskInfo {
    std::uint32_t number = 0;
    std::optional<std::uint64_t> total_bytes;
    unsigned partition_style = 0;
    std::wstring disk_guid; // Empty unless a nonzero GPT disk ID is present.
    std::optional<std::uint32_t> mbr_signature;
    std::vector<PartitionInfo> partitions;
    std::wstring query_error;
};

struct ImageInfo {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
    std::wstring pool_id;
    std::uint8_t member_index = 0;
    std::uint8_t member_count = 0;
    bool encrypted = false;
};

struct ImageScanResult {
    std::vector<ImageInfo> images;
    std::vector<std::wstring> errors;
    bool truncated = false;
};

std::vector<VolumeInfo> enumerate_volumes();
std::vector<DiskInfo> enumerate_disks();
std::vector<std::filesystem::path> default_image_roots(
    const std::vector<VolumeInfo>& volumes);

// An empty roots list scans each mounted volume's PoolFS folder. Otherwise,
// only the supplied files/folders are inspected. Recursive scanning never
// descends into directory symlinks beneath those roots. The walk is capped by
// max_entries and max_depth.
ImageScanResult scan_images(const std::vector<std::filesystem::path>& roots,
                            bool recursive = false,
                            std::size_t max_entries = 4096,
                            std::size_t max_depth = 4);

} // namespace poolfs::gui
