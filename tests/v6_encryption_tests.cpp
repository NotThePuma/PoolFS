#include "core.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kMemberBytes = 384 * kMiB;
constexpr std::uint64_t kSectorBytes = 4096;
constexpr std::uint64_t kCheckpointStart = 2 * kSectorBytes + 64 * kMiB;
constexpr std::string_view kPassword = "PoolFS-test-password-only-2026";
constexpr std::string_view kWrongPassword = "PoolFS-test-password-only-wrong";
constexpr std::string_view kSecretPath =
    "private/secret_filename_8c3f2b84d59e4b6f.bin";
constexpr std::string_view kSecretText =
    "PoolFS encrypted test content: unreadable without the unlock password. ";

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Files {
    fs::path folder;
    std::vector<fs::path> members;
    Files() = default;
    Files(const Files&) = delete;
    Files& operator=(const Files&) = delete;
    Files(Files&& other) noexcept
        : folder(std::move(other.folder)), members(std::move(other.members)) {
        other.folder.clear();
        other.members.clear();
    }
    ~Files() {
        for (const auto& path : members) {
            std::error_code ignored;
            fs::remove(path, ignored);
        }
        if (!folder.empty()) {
            std::error_code ignored;
            fs::remove(folder, ignored);
        }
    }
};

Files fresh_members(std::string_view suffix) {
    const auto root = fs::absolute("work");
    fs::create_directories(root);
    const auto token = std::to_string(GetCurrentProcessId()) + "-" +
        std::to_string(GetTickCount64()) + "-" + std::string(suffix);
    Files files;
    files.folder = root / ("v6-encryption-tests-" + token);
    require(fs::create_directory(files.folder), "test folder already exists");
    files.members = {files.folder / "member-a.img", files.folder / "member-b.img"};
    return files;
}

std::array<std::byte, 4096> sector(const fs::path& path, std::uint64_t offset) {
    std::array<std::byte, 4096> bytes{};
    std::ifstream stream(path, std::ios::binary);
    require(stream.good(), "cannot open member for raw sector read");
    stream.seekg(static_cast<std::streamoff>(offset));
    stream.read(reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));
    require(stream.gcount() == static_cast<std::streamsize>(bytes.size()),
            "short raw sector read");
    return bytes;
}

bool contains(const fs::path& path, std::string_view needle) {
    require(!needle.empty(), "empty raw scan needle");
    std::ifstream input(path, std::ios::binary);
    require(input.good(), "cannot open member for raw scan");
    constexpr std::size_t kChunk = 1024 * 1024;
    std::vector<char> buffer(kChunk + needle.size() - 1);
    std::size_t carry = 0;
    for (;;) {
        input.read(buffer.data() + carry, static_cast<std::streamsize>(kChunk));
        const auto count = static_cast<std::size_t>(input.gcount());
        if (!count) break;
        const auto end = buffer.begin() + carry + count;
        if (std::search(buffer.begin(), end, needle.begin(), needle.end()) != end)
            return true;
        carry = std::min(needle.size() - 1, carry + count);
        std::copy(end - carry, end, buffer.begin());
    }
    require(input.eof(), "raw image scan failed");
    return false;
}

void expect_access_denied(const std::vector<fs::path>& members,
                          std::string_view password, bool supplied) {
    try {
        if (supplied) {
            poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, members, password);
            (void)pool.List();
        } else {
            poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, members);
            (void)pool.List();
        }
    } catch (const poolfs::Error& error) {
        require(error.code() == poolfs::ErrorCode::AccessDenied,
                "unlock failed for a reason other than access denied");
        return;
    }
    throw std::runtime_error("pool opened without the correct password");
}

void test_encrypted_pool() {
    auto files = fresh_members("encrypted");
    poolfs::Pool::FormatV6EncryptedImages(files.members, kMemberBytes, kPassword);
    require(poolfs::Pool::V6Encrypted(files.members.front()),
            "encrypted format marker missing");
    expect_access_denied(files.members, {}, false);
    expect_access_denied(files.members, kWrongPassword, true);

    std::vector<std::byte> expected(3 * kSectorBytes + 173);
    for (std::size_t i = 0; i < expected.size(); ++i)
        expected[i] = static_cast<std::byte>((i * 73 + 19) & 255);
    std::copy(kSecretText.begin(), kSecretText.end(),
              reinterpret_cast<char*>(expected.data()));
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, files.members, kPassword);
        pool.MakeDirectory("private");
        pool.Create(std::string(kSecretPath));
        require(pool.Write(std::string(kSecretPath), 0, expected) == expected.size(),
                "multi-sector write was short");
        std::array<std::byte, 137> patch{};
        patch.fill(std::byte{0xA7});
        require(pool.Write(std::string(kSecretPath), 7000, patch) == patch.size(),
                "partial-sector write was short");
        std::copy(patch.begin(), patch.end(), expected.begin() + 7000);
        pool.Flush();
    }
    std::reverse(files.members.begin(), files.members.end());
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, files.members, kPassword);
        const auto entry = pool.Stat(std::string(kSecretPath));
        require(entry.size == expected.size(), "reopened file size differs");
        std::vector<std::byte> actual(expected.size());
        require(pool.Read(std::string(kSecretPath), 0, actual) == actual.size(),
                "reopened file read was short");
        require(actual == expected, "reopened plaintext differs");
        require(pool.Scrub([](const poolfs::ScrubIssue&) {}) == 0,
                "encrypted pool scrub found damage");
    }
    expect_access_denied(files.members, kWrongPassword, true);
    for (const auto& member : files.members) {
        require(!contains(member, kSecretPath), "filename leaked into member image");
        require(!contains(member, kSecretText), "file content leaked into member image");
        require(!contains(member, kPassword), "password leaked into member image");
    }
    const auto first = sector(files.members[0], kCheckpointStart);
    const auto second = sector(files.members[1], kCheckpointStart);
    require(first != second, "equal checkpoint plaintext has equal member ciphertext");
    constexpr std::string_view kCheckpointMagic = "PFSCP006";
    require(!std::equal(kCheckpointMagic.begin(), kCheckpointMagic.end(), first.begin(),
                        [](char left, std::byte right) {
                            return left == static_cast<char>(right);
                        }), "checkpoint header remains plaintext");
    require(!std::equal(kCheckpointMagic.begin(), kCheckpointMagic.end(), second.begin(),
                        [](char left, std::byte right) {
                            return left == static_cast<char>(right);
                        }), "second checkpoint header remains plaintext");
}

void test_plain_v6_compatibility() {
    auto files = fresh_members("plaintext");
    poolfs::Pool::FormatV6Images(files.members, kMemberBytes);
    require(!poolfs::Pool::V6Encrypted(files.members.front()),
            "unencrypted format unexpectedly requires a password");
    poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, files.members);
    pool.Create("legacy-layout-still-opens.txt");
    require(pool.Stat("legacy-layout-still-opens.txt").size == 0,
            "unencrypted v6 layout did not open");
}

} // namespace

int main() {
    try {
        test_encrypted_pool();
        test_plain_v6_compatibility();
        std::cout << "PoolFS v6 encryption tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PoolFS v6 encryption tests failed: " << error.what() << '\n';
        return 1;
    }
}
