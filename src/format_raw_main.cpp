#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "core.h"
#include "member_device.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::size_t kMaximumMembers = 16;

void usage() {
    std::wcerr << L"Usage: poolfs-format-raw <volume-1> <disk-guid-1> "
               << L"<volume-2> <disk-guid-2> --inspect|--erase\n"
               << L"   or: poolfs-format-raw --v3 --inspect|--erase "
               << L"<volume> <disk-guid> <partition-guid> [repeat 2..16 times]\n"
               << L"Volume paths must be \\\\?\\Volume{GUID} without a final backslash.\n";
}

void validate_v3_targets(const std::vector<std::unique_ptr<poolfs::RawVolume>>& volumes) {
    for (std::size_t i = 0; i < volumes.size(); ++i) {
        const auto& a = volumes[i]->info();
        if (a.partition_start_bytes >
            std::numeric_limits<std::uint64_t>::max() - a.partition_length_bytes)
            throw poolfs::Error(poolfs::ErrorCode::InvalidArgument,
                                "raw partition extent overflows");
        const auto a_end = a.partition_start_bytes + a.partition_length_bytes;
        for (std::size_t j = 0; j < i; ++j) {
            const auto& b = volumes[j]->info();
            if (a.partition_guid == b.partition_guid)
                throw poolfs::Error(poolfs::ErrorCode::InvalidArgument,
                                    "raw partition GUIDs must be distinct");
            if (a.disk_number != b.disk_number) continue;
            if (b.partition_start_bytes >
                std::numeric_limits<std::uint64_t>::max() - b.partition_length_bytes)
                throw poolfs::Error(poolfs::ErrorCode::InvalidArgument,
                                    "raw partition extent overflows");
            const auto b_end = b.partition_start_bytes + b.partition_length_bytes;
            if (a.partition_start_bytes < b_end && b.partition_start_bytes < a_end)
                throw poolfs::Error(poolfs::ErrorCode::InvalidArgument,
                                    "raw partition extents overlap");
        }
    }
}

} // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    const bool v3 = argc >= 2 && std::wstring_view(argv[1]) == L"--v3";
    const bool inspect = v3 ? (argc >= 3 && std::wstring_view(argv[2]) == L"--inspect") :
        (argc == 6 && std::wstring_view(argv[5]) == L"--inspect");
    const bool erase = v3 ? (argc >= 3 && std::wstring_view(argv[2]) == L"--erase") :
        (argc == 6 && std::wstring_view(argv[5]) == L"--erase");
    const bool valid_v3_count = v3 && argc >= 9 && argc <= 3 + 3 * static_cast<int>(kMaximumMembers) &&
        (argc - 3) % 3 == 0;
    if ((!inspect && !erase) || (v3 && !valid_v3_count)) {
        usage();
        LocalFree(argv);
        return 2;
    }
    std::vector<poolfs::RawMemberSpec> members;
    if (v3) {
        members.reserve(static_cast<std::size_t>((argc - 3) / 3));
        for (int i = 3; i < argc; i += 3)
            members.push_back(poolfs::RawMemberSpec{argv[i], argv[i + 1], argv[i + 2]});
    } else {
        members.push_back(poolfs::RawMemberSpec{argv[1], argv[2], L""});
        members.push_back(poolfs::RawMemberSpec{argv[3], argv[4], L""});
    }
    LocalFree(argv);
    try {
        if (inspect) {
            std::vector<std::unique_ptr<poolfs::RawVolume>> volumes;
            volumes.reserve(members.size());
            for (const auto& spec : members)
                volumes.push_back(std::make_unique<poolfs::RawVolume>(
                    spec.volume_path, spec.expected_disk_guid, spec.expected_partition_guid));
            if (v3) validate_v3_targets(volumes);
            else if (volumes[0]->info().disk_number == volumes[1]->info().disk_number)
                throw poolfs::Error(poolfs::ErrorCode::InvalidArgument,
                                    "raw members must be on separate physical disks");
            std::array<std::byte, 8192> header{};
            std::array<std::byte, 4096> tail{};
            for (const auto& volume : volumes) {
                auto& member = *volume;
                if (member.size() < poolfs::Pool::kMinimumRawBytes)
                    throw poolfs::Error(poolfs::ErrorCode::InvalidArgument,
                                        "raw member is smaller than PoolFS minimum");
                member.read(0, header.data(), header.size());
                member.read(member.size() - tail.size(), tail.data(), tail.size());
                const bool blank = std::all_of(header.begin(), header.end(),
                    [](std::byte value) { return value == std::byte{0}; });
                std::wcout << member.info().volume_path << L" | disk "
                           << member.info().disk_number << L" | GPT "
                           << member.info().disk_guid;
                if (v3)
                    std::wcout << L" | partition " << member.info().partition_guid
                               << L" @ " << member.info().partition_start_bytes
                               << L" + " << member.info().partition_length_bytes;
                std::wcout << L" | bytes "
                           << member.info().bytes << L" | first 8192 bytes "
                           << (blank ? L"blank" : L"nonblank")
                           << L" | usable-end read OK\n";
            }
            return 0;
        }
        if (v3) {
            poolfs::Pool::FormatRawV3(members);
            std::wcout << L"Formatted " << members.size()
                       << L" raw partitions as a PoolFS v3 pool.\n";
        } else {
            poolfs::Pool::FormatRaw(members[0], members[1]);
            std::wcout << L"Formatted two dedicated raw partitions as a PoolFS v2 pool.\n";
        }
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "poolfs-format-raw: " << error.what() << '\n';
        return 1;
    }
}
