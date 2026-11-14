#include "core.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::string_view kPassword = "PoolFS-portable-test-password-2026";

void require(bool condition, std::string_view message) {
    if (!condition) throw std::runtime_error(std::string(message));
}

struct Fixture {
    fs::path root;
    std::vector<fs::path> folders;
    std::vector<fs::path> files;

    Fixture() = default;
    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
    Fixture(Fixture&& other) noexcept
        : root(std::move(other.root)), folders(std::move(other.folders)),
          files(std::move(other.files)) {
        other.root.clear();
        other.folders.clear();
        other.files.clear();
    }
    ~Fixture() {
        for (const auto& file : files) {
            std::error_code ignored;
            fs::remove(file, ignored);
        }
        for (auto folder = folders.rbegin(); folder != folders.rend(); ++folder) {
            std::error_code ignored;
            fs::remove(*folder, ignored);
        }
        if (!root.empty()) {
            std::error_code ignored;
            fs::remove(root, ignored);
        }
    }

    fs::path folder(const fs::path& name) {
        const auto path = root / name;
        require(fs::create_directory(path), "portable test folder already exists");
        folders.push_back(path);
        return path;
    }
    fs::path image(const fs::path& directory, const fs::path& name) {
        const auto path = directory / name;
        require(!fs::exists(path), "portable test image path already exists");
        files.push_back(path);
        return path;
    }
};

Fixture fresh_fixture(std::string_view name) {
    const auto workspace = fs::absolute("work");
    fs::create_directories(workspace);
    Fixture fixture;
    fixture.root = workspace / ("v6-portable-" + std::string(name) + "-" +
        std::to_string(GetCurrentProcessId()) + "-" + std::to_string(GetTickCount64()));
    if (!fs::create_directory(fixture.root)) {
        fixture.root.clear();
        throw std::runtime_error("portable test root already exists");
    }
    return fixture;
}

std::vector<std::byte> contents() {
    std::vector<std::byte> result(2 * 4096 + 137);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::byte>((i * 61 + 23) & 255);
    return result;
}

void create_file(poolfs::Pool& pool, const std::vector<std::byte>& expected) {
    pool.MakeDirectory("private");
    pool.Create("private/transport.bin");
    require(pool.Write("private/transport.bin", 0, expected) == expected.size(),
            "portable test write was short");
    pool.Flush();
}

void verify_file(poolfs::Pool& pool, const std::vector<std::byte>& expected) {
    require(pool.Stat("private/transport.bin").size == expected.size(),
            "portable test file size changed");
    std::vector<std::byte> actual(expected.size());
    require(pool.Read("private/transport.bin", 0, actual) == actual.size(),
            "portable test read was short");
    require(actual == expected, "portable test file contents changed");
    require(pool.Scrub({}) == 0, "portable test scrub found damage");
}

void copy_then_remove(const fs::path& source, const fs::path& destination) {
    require(fs::copy_file(source, destination), "member image copy failed");
    require(fs::file_size(source) == fs::file_size(destination),
            "member image length changed during copy");
    require(fs::remove(source), "could not remove original member after copy");
}

void expect_wrong_password(const std::vector<fs::path>& members) {
    try {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, members,
                          "wrong-portable-test-password");
        (void)pool.List();
    } catch (const poolfs::Error& error) {
        require(error.code() == poolfs::ErrorCode::AccessDenied,
                "wrong password failed for an unexpected reason");
        return;
    }
    throw std::runtime_error("wrong password opened the portable pool");
}

void test_one_encrypted_member() {
    auto fixture = fresh_fixture("one-encrypted");
    const auto original = fixture.image(fixture.folder("source"), "part-a.img");
    const auto moved = fixture.image(fixture.folder(fs::path(L"moved-\u00fc")),
                                     "renamed-part.img");
    poolfs::Pool::FormatV6EncryptedImages({original}, 384 * kMiB, kPassword);
    require(poolfs::Pool::V6Encrypted(original), "one-member encrypted marker missing");
    const auto expected = contents();
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {original}, kPassword);
        bool second_writer_denied = false;
        try {
            poolfs::Pool concurrent(poolfs::Pool::V6ImageTag{}, {original}, kPassword);
            (void)concurrent.List();
        } catch (const poolfs::Error& error) {
            second_writer_denied = error.code() == poolfs::ErrorCode::Io;
        }
        require(second_writer_denied, "member image allowed concurrent writable opens");
        create_file(pool, expected);
    }
    expect_wrong_password({original});
    copy_then_remove(original, moved);
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {moved}, kPassword);
        verify_file(pool, expected);
    }
    expect_wrong_password({moved});
}

void test_one_plain_member() {
    auto fixture = fresh_fixture("one-plain");
    const auto original = fixture.image(fixture.folder("source"), "plain-a.img");
    const auto moved = fixture.image(fixture.folder("destination"), "plain-renamed.img");
    poolfs::Pool::FormatV6Images({original}, 384 * kMiB);
    require(!poolfs::Pool::V6Encrypted(original), "one-member plain pool is encrypted");
    const auto expected = contents();
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {original});
        create_file(pool, expected);
    }
    copy_then_remove(original, moved);
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {moved});
        verify_file(pool, expected);
    }
}

void test_two_moved_members() {
    auto fixture = fresh_fixture("two-encrypted");
    const auto first = fixture.image(fixture.folder("host-one"), "first.img");
    const auto second = fixture.image(fixture.folder("host-two"), "second.img");
    const auto moved_first = fixture.image(fixture.folder("other-host-one"), "moved-first.img");
    const auto moved_second = fixture.image(fixture.folder("other-host-two"), "moved-second.img");
    poolfs::Pool::FormatV6EncryptedImages(std::vector<poolfs::ImageMemberSpec>{
        {first, 384 * kMiB}, {second, 512 * kMiB}}, kPassword);
    const auto expected = contents();
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {first, second}, kPassword);
        create_file(pool, expected);
    }
    copy_then_remove(first, moved_first);
    copy_then_remove(second, moved_second);
    try {
        poolfs::Pool incomplete(poolfs::Pool::V6ImageTag{}, {moved_first}, kPassword);
        (void)incomplete.List();
        throw std::runtime_error("two-member pool opened with a missing member");
    } catch (const poolfs::Error& error) {
        require(error.code() == poolfs::ErrorCode::Corrupt,
                "missing pool member was not reported as corrupt/incomplete");
    }
    // Member paths and argument order are independent of the on-disk manifest.
    {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{},
                          {moved_second, moved_first}, kPassword);
        verify_file(pool, expected);
    }
}

void test_grow_copy(bool encrypted) {
    auto fixture = fresh_fixture(encrypted ? "grow-encrypted" : "grow-plain");
    const auto folder = fixture.folder("members");
    const auto first = fixture.image(folder, "first.img");
    const auto second = fixture.image(folder, "second.img");
    const auto grown_first = fixture.image(folder, "first-grown.img");
    const auto grown_second = fixture.image(folder, "second-grown.img");
    const std::vector<poolfs::ImageMemberSpec> initial = {
        {first, 384 * kMiB}, {second, 384 * kMiB}};
    if (encrypted) poolfs::Pool::FormatV6EncryptedImages(initial, kPassword);
    else poolfs::Pool::FormatV6Images(initial);
    const auto expected = contents();
    std::uint64_t old_capacity = 0;
    if (encrypted) {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {first, second}, kPassword);
        create_file(pool, expected);
        old_capacity = pool.Capacity();
    } else {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, {first, second});
        create_file(pool, expected);
        old_capacity = pool.Capacity();
    }
    poolfs::Pool::GrowV6Images({
        {second, grown_second, (encrypted ? 512 : 384) * kMiB},
        {first, grown_first, 640 * kMiB}},
        encrypted ? kPassword : std::string_view{});
    require(fs::file_size(first) == 384 * kMiB &&
            fs::file_size(second) == 384 * kMiB,
            "grow changed original images");
    if (encrypted) {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{},
                          {grown_second, grown_first}, kPassword);
        require(pool.Capacity() > old_capacity, "grown pool capacity did not increase");
        verify_file(pool, expected);
        poolfs::Pool original(poolfs::Pool::V6ReadOnlyImageTag{},
                              {first, second}, kPassword);
        verify_file(original, expected);
    } else {
        poolfs::Pool pool(poolfs::Pool::V6ImageTag{},
                          {grown_second, grown_first});
        require(pool.Capacity() > old_capacity, "grown pool capacity did not increase");
        verify_file(pool, expected);
        poolfs::Pool original(poolfs::Pool::V6ReadOnlyImageTag{}, {first, second});
        verify_file(original, expected);
    }
}

} // namespace

int main() {
    try {
        test_one_encrypted_member();
        test_one_plain_member();
        test_two_moved_members();
        test_grow_copy(false);
        test_grow_copy(true);
        std::cout << "PoolFS v6 portable member tests passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "PoolFS v6 portable member tests failed: " << error.what() << '\n';
        return 1;
    }
}
