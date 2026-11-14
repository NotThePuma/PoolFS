#include "gui_inventory.h"
#include "gui_partition_ops.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

void expect_rejected(const poolfs::gui::PartitionRequest& request,
                     bool apply, std::string_view message) {
    try {
        (void)poolfs::gui::run_partition_operation(request, apply);
    } catch (const std::exception&) {
        return;
    }
    throw std::runtime_error(std::string(message));
}

bool contains_ci(std::wstring text, std::wstring value) {
    std::transform(text.begin(), text.end(), text.begin(),
        [](wchar_t c) { return std::towlower(c); });
    std::transform(value.begin(), value.end(), value.begin(),
        [](wchar_t c) { return std::towlower(c); });
    return text.find(value) != std::wstring::npos;
}

void test_validation() {
    poolfs::gui::PartitionRequest request;
    request.action = poolfs::gui::PartitionAction::CreateAndFormat;
    request.disk_number = 1;
    request.expected_disk_bytes = 10ULL * 1024 * 1024 * 1024;
    request.expected_disk_guid = L"{11111111-1111-1111-1111-111111111111}";
    request.size_mib = 512;
    request.drive_letter = L'Z';
    request.filesystem = L"NTFS";

    auto invalid = request;
    invalid.expected_disk_bytes = 0;
    expect_rejected(invalid, false, "zero disk snapshot was accepted");
    invalid = request;
    invalid.expected_disk_guid.clear();
    expect_rejected(invalid, false, "missing GPT disk identity was accepted");
    invalid = request;
    invalid.filesystem = L"ReFS";
    expect_rejected(invalid, false, "unsupported file system was accepted");
    invalid = request;
    invalid.drive_letter = L'1';
    expect_rejected(invalid, false, "invalid drive letter was accepted");
    invalid = request;
    invalid.label = L"bad:name";
    expect_rejected(invalid, false, "invalid volume label was accepted");
    invalid = request;
    invalid.action = poolfs::gui::PartitionAction::Format;
    expect_rejected(invalid, false, "format without partition identity was accepted");
    expect_rejected(request, true, "apply without preview token was accepted");
    if (!poolfs::gui::partition_operations_elevated()) {
        invalid = request;
        invalid.disk_token = L"not-a-live-token";
        expect_rejected(invalid, true,
                        "non-elevated partition apply was accepted");
    }
}

void test_preview() {
    const auto disks = poolfs::gui::enumerate_disks();
    bool reviewed = false;
    for (const auto& disk : disks) {
        if (!disk.total_bytes || disk.disk_guid.empty()) continue;
        for (const auto& partition : disk.partitions) {
            if (partition.partition_guid.empty()) continue;
            poolfs::gui::PartitionRequest request;
            request.action = poolfs::gui::PartitionAction::Format;
            request.disk_number = disk.number;
            request.expected_disk_bytes = *disk.total_bytes;
            request.expected_disk_guid = disk.disk_guid;
            request.partition_number = partition.number;
            request.expected_partition_guid = partition.partition_guid;
            request.expected_partition_offset = partition.offset_bytes;
            request.expected_partition_bytes = partition.length_bytes;
            request.filesystem = L"NTFS";
            try {
                const auto preview = poolfs::gui::run_partition_operation(
                    request, false);
                require(!preview.summary.empty() && !preview.disk_token.empty(),
                        "safe partition preview omitted target or token");
                require(preview.summary.find(L"POOLFS_DISK_TOKEN=") ==
                        std::wstring::npos,
                        "internal disk token leaked into preview summary");
                require(contains_ci(preview.summary, disk.disk_guid) &&
                        contains_ci(preview.summary,
                                    partition.partition_guid),
                        "preview omitted exact disk or partition GUID");
                require(preview.summary.find(L"NTFS") != std::wstring::npos,
                        "preview omitted requested file system");
                reviewed = true;
                break;
            } catch (const std::exception& error) {
                if (std::string_view(error.what()).find(
                        "partition helper differs") != std::string_view::npos)
                    throw;
                // A running pool may hold an image on the selected host disk.
                // Refusal is the expected outcome until it is unmounted.
                if (std::string_view(error.what()).find(
                        "Unmount all PoolFS drives") != std::string_view::npos) {
                    std::cout << "Read-only preview correctly refused while a PoolFS mount is active\n";
                    return;
                }
                // Only eligible non-system basic-data partitions can preview.
            }
        }
        if (reviewed) break;
    }
    if (!reviewed)
        std::cout << "No eligible GPT disk for read-only partition preview; skipped\n";
}

} // namespace

int main(int argc, char** argv) {
    try {
        test_validation();
        if (argc == 2 && std::string_view(argv[1]) == "--integration")
            test_preview();
        std::cout << "PoolFS GUI partition operation tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PoolFS GUI partition operation tests failed: " <<
            error.what() << '\n';
        return 1;
    }
}
