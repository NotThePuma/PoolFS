// Offline, read-only integrity scan for PoolFS image members.
#include "core.h"
#include "password.h"

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <memory>
#include <vector>

namespace {

int run(int argc, char** argv) {
    if (argc < 2 || argc > 17) {
        std::cerr << "Usage: poolfs-scrub <member.img> [up to 16 members]\n";
        return 1;
    }

    std::vector<std::filesystem::path> paths;
    paths.reserve(static_cast<std::size_t>(argc - 1));
    for (int i = 1; i < argc; ++i)
        paths.push_back(std::filesystem::absolute(argv[i]).lexically_normal());

    // The read-only constructor opens every image exclusively and never
    // retires an inactive checkpoint while inspecting it.
    std::unique_ptr<poolfs::Pool> pool;
    if (poolfs::Pool::V6Encrypted(paths.front())) {
        const auto secret = poolfs::password::prompt(L"PoolFS password: ");
        pool = std::make_unique<poolfs::Pool>(
            poolfs::Pool::V6ReadOnlyImageTag{}, paths, secret.view());
    } else {
        pool = std::make_unique<poolfs::Pool>(poolfs::Pool::V6ReadOnlyImageTag{}, paths);
    }
    const auto entries = pool->List();
    const auto file_count = std::count_if(entries.begin(), entries.end(),
        [](const poolfs::Entry& entry) { return !entry.is_directory; });
    std::cout << "poolfs-scrub: " << file_count << " file(s) scanned\n";
    constexpr std::uint64_t kMaximumReportedIssues = 1000;
    std::uint64_t printed = 0;
    const auto count = pool->Scrub([&](const poolfs::ScrubIssue& issue) {
        if (printed >= kMaximumReportedIssues) return;
        std::cout << "  damaged: file=" << issue.name
                  << " member=" << static_cast<unsigned>(issue.member)
                  << " logical=" << issue.logical
                  << " physical=" << issue.physical << '\n';
        ++printed;
    });
    if (count > printed)
        std::cout << "  ... " << count - printed << " more damaged block(s)\n";
    if (count) {
        std::cout << "status: DAMAGED (" << count
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
