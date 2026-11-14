#include "core.h"
#include "password.h"

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr int kMaximumPooledMembers = 16;

std::uint64_t minimum_mib(int version) {
    return version == 6 ? poolfs::Pool::kMinimumV6ImageBytes / kMiB :
           version == 5 ? poolfs::Pool::kMinimumV5ImageBytes / kMiB :
           version == 4 ? poolfs::Pool::kMinimumV4ImageBytes / kMiB :
           version == 3 ? poolfs::Pool::kMinimumRawBytes / kMiB :
                          poolfs::Pool::kMinimumImageBytes / kMiB;
}

bool parse_size_mib(std::string_view text, int version, std::uint64_t& size_mib) {
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), size_mib);
    return !text.empty() && parsed.ec == std::errc{} &&
           parsed.ptr == text.data() + text.size() &&
           size_mib >= minimum_mib(version) &&
           size_mib <= std::numeric_limits<std::int64_t>::max() / kMiB;
}

void print_size_error(int version, std::size_t member = 0) {
    if (member) std::cerr << "member " << member << ' ';
    std::cerr << "size-MiB must be an integer from " << minimum_mib(version)
              << " through " << std::numeric_limits<std::int64_t>::max() / kMiB
              << "\n";
}

void print_usage() {
    std::cerr << "Usage: poolfs-format <size-MiB> <member.img> [up to 16 members]\n"
              << "   or: poolfs-format --sized <size-MiB> <member.img> [repeat 1..16 times]\n"
              << "   or: poolfs-format --encrypted <size-MiB> <member.img> [up to 16 members]\n"
              << "   or: poolfs-format --encrypted-sized <size-MiB> <member.img> [repeat 1..16 times]\n"
              << "   or: poolfs-format --grow-copy <old.img> <new-size-MiB> <new.img> "
                 "[repeat for every member]\n"
              << "Legacy v3-v5 modes still require at least two members.\n";
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view mode = argc >= 2 ? std::string_view(argv[1]) : "";
    if (mode == "--grow-copy") {
        if (argc < 5 || argc > 2 + 3 * kMaximumPooledMembers ||
            (argc - 2) % 3 != 0) {
            print_usage();
            return 2;
        }
        std::vector<poolfs::ImageGrowSpec> members;
        for (int i = 2; i < argc; i += 3) {
            std::uint64_t size_mib = 0;
            if (!parse_size_mib(argv[i + 1], 6, size_mib)) {
                print_size_error(6, members.size() + 1);
                return 2;
            }
            members.push_back({std::filesystem::path(argv[i]),
                               std::filesystem::path(argv[i + 2]), size_mib * kMiB});
        }
        try {
            const bool encrypted = poolfs::Pool::V6Encrypted(members.front().source);
            const auto secret = encrypted ?
                std::optional<poolfs::password::Secret>(
                    poolfs::password::prompt(L"PoolFS password: ")) : std::nullopt;
            poolfs::Pool::GrowV6Images(members, secret ? secret->view() : std::string_view{});
            std::cout << "Expanded and verified " << members.size()
                      << " new member copies. Originals were not changed.\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Grow failed: " << error.what() << '\n';
            return 1;
        }
    }
    const bool v3 = mode == "--v3" || mode == "--v3-sized";
    const bool v4 = mode == "--v4" || mode == "--v4-sized";
    const bool v5 = mode == "--v5" || mode == "--v5-sized";
    const bool v6_alias = mode == "--v6" || mode == "--v6-sized";
    const bool encrypted = mode == "--encrypted" || mode == "--encrypted-sized";
    const bool sized = mode == "--sized" || mode == "--v3-sized" ||
                       mode == "--v4-sized" || mode == "--v5-sized" ||
                       mode == "--v6-sized" || mode == "--encrypted-sized";
    const bool named_mode = v3 || v4 || v5 || v6_alias || encrypted || mode == "--sized";
    const bool unknown_flag = !mode.empty() && mode.front() == '-' && !named_mode;
    const int version = v5 ? 5 : v4 ? 4 : v3 ? 3 : 6;
    const int minimum_members = version == 6 ? 1 : 2;
    const int size_arg = named_mode ? 2 : 1;
    const int first_member_arg = size_arg + 1;
    if ((sized && (argc < 2 + 2 * minimum_members || argc > 2 + 2 * kMaximumPooledMembers ||
                   (argc - 2) % 2 != 0)) ||
        (!sized && (argc < first_member_arg + minimum_members ||
                    argc > first_member_arg + kMaximumPooledMembers)) ||
        unknown_flag) {
        print_usage();
        return 2;
    }

    if (sized) {
        std::vector<poolfs::ImageMemberSpec> members;
        members.reserve(static_cast<std::size_t>((argc - 2) / 2));
        for (int i = 2; i < argc; i += 2) {
            std::uint64_t member_size_mib = 0;
            if (!parse_size_mib(argv[i], version, member_size_mib)) {
                print_size_error(version, members.size() + 1);
                return 2;
            }
            members.push_back({std::filesystem::path(argv[i + 1]), member_size_mib * kMiB});
        }
        try {
            if (v5) poolfs::Pool::FormatV5Images(members);
            else if (v4) poolfs::Pool::FormatV4Images(members);
            else if (v3) poolfs::Pool::FormatV3Images(members);
            else if (encrypted) {
                const auto secret = poolfs::password::prompt(L"New PoolFS password: ", true);
                if (secret.view().size() < 12)
                    throw std::runtime_error("PoolFS password must be at least 12 UTF-8 bytes");
                poolfs::Pool::FormatV6EncryptedImages(members, secret.view());
            }
            else poolfs::Pool::FormatV6Images(members);
            std::cout << "Created " << members.size() << " pool members with individual sizes:";
            for (const auto& member : members)
                std::cout << ' ' << member.bytes / kMiB << " MiB";
            std::cout << ".\n";
            return 0;
        } catch (const std::exception& error) {
            std::cerr << "Format failed: " << error.what() << '\n';
            return 1;
        }
    }

    std::uint64_t size_mib = 0;
    if (!parse_size_mib(argv[size_arg], version, size_mib)) {
        print_size_error(version);
        return 2;
    }

    try {
        std::vector<std::filesystem::path> members;
        members.reserve(static_cast<std::size_t>(argc - first_member_arg));
        for (int i = first_member_arg; i < argc; ++i)
            members.emplace_back(argv[i]);
        // The core checks every normalized path for duplicates and
        // refuses to overwrite existing files before creating any member.
        if (v5)
            poolfs::Pool::FormatV5Images(members, size_mib * kMiB);
        else if (v4)
            poolfs::Pool::FormatV4Images(members, size_mib * kMiB);
        else if (v3)
            poolfs::Pool::FormatV3Images(members, size_mib * kMiB);
        else if (encrypted) {
            const auto secret = poolfs::password::prompt(L"New PoolFS password: ", true);
            if (secret.view().size() < 12)
                throw std::runtime_error("PoolFS password must be at least 12 UTF-8 bytes");
            poolfs::Pool::FormatV6EncryptedImages(members, size_mib * kMiB, secret.view());
        }
        else
            poolfs::Pool::FormatV6Images(members, size_mib * kMiB);
        std::cout << "Created " << members.size() << " pool members of "
                  << size_mib << " MiB each.\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Format failed: " << e.what() << '\n';
        return 1;
    }
}
