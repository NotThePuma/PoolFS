#pragma once

#include <cstdint>
#include <string>

namespace poolfs::gui {

enum class PartitionAction {
    InitializeGpt,
    CreateAndFormat,
    Format,
    Remove,
    ChangeLetter
};

// The target fields come from a recent read-only disk inventory. Preview
// returns a disk token; apply requires that same token and rechecks every
// identity and safety guard before changing the disk.
struct PartitionRequest {
    PartitionAction action = PartitionAction::CreateAndFormat;
    std::uint32_t disk_number = 0;
    std::uint64_t expected_disk_bytes = 0;
    std::wstring expected_disk_guid;
    std::uint32_t partition_number = 0;
    std::wstring expected_partition_guid;
    std::uint64_t expected_partition_offset = 0;
    std::uint64_t expected_partition_bytes = 0;
    std::uint64_t size_mib = 0; // 0 means all available space when creating.
    wchar_t drive_letter = L'\0';
    std::wstring filesystem = L"NTFS";
    std::wstring label;
    std::wstring disk_token;
};

struct PartitionResult {
    std::wstring summary;
    std::wstring disk_token;
};

bool partition_operations_elevated();

// apply=false performs read-only validation and returns a disk token. The UI
// must show summary and ask for confirmation before calling with apply=true.
// apply=true requires the token from preview and administrator elevation.
PartitionResult run_partition_operation(const PartitionRequest& request,
                                        bool apply);

} // namespace poolfs::gui
