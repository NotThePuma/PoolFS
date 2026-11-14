// Offline, read-only data-integrity scan for PoolFS v5 image members.
#include "core.h"

#include <cstdint>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace {

int run(int argc, char** argv) {
    if (argc < 3 || argc > 17) {
        std::cerr << "Usage: poolfs-scrub <member1.img> <member2.img> "
                     "[up to 16 members]\n";
        return 1;
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i)
        paths.push_back(std::filesystem::absolute(argv[i]).lexically_normal());

    // The read-only pool opens every image exclusively so the namespace and
    // checksum table cannot change while the scan is in progress.
    poolfs::Pool pool(poolfs::Pool::V5ReadOnlyImageTag{}, paths);
    const auto file_count = pool.List().size();
    std::cout << "poolfs-scrub: " << file_count << " file(s) scanned\n";
    constexpr std::uint64_t kMaximumReportedIssues = 1000;
    std::uint64_t printed = 0;
    const auto issue_count = pool.Scrub([&](const poolfs::ScrubIssue& issue) {
        if (printed >= kMaximumReportedIssues) return;
        std::cout << "  damaged: file=" << issue.name
                  << " member=" << static_cast<unsigned>(issue.member)
                  << " logical=" << issue.logical
                  << " physical=" << issue.physical << '\n';
        ++printed;
    });
    if (issue_count > printed)
        std::cout << "  ... " << issue_count - printed << " more damaged block(s)\n";
    if (issue_count) {
        std::cout << "status: DAMAGED (" << issue_count
                  << " block(s)); original bytes cannot be recovered without redundancy\n";
        return 2;
    }
    std::cout << "status: CLEAN\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return run(argc, argv);
    } catch (const poolfs::Error& error) {
        std::cerr << "status: "
                  << (error.code() == poolfs::ErrorCode::Corrupt ? "CORRUPT" : "ERROR")
                  << "\nreason: " << error.what() << '\n';
        return error.code() == poolfs::ErrorCode::Corrupt ? 2 : 1;
    } catch (const std::exception& error) {
        std::cerr << "status: ERROR\nreason: " << error.what() << '\n';
        return 1;
    }
}
