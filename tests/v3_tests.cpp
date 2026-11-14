#include "core.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using poolfs::Pool;

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error("v3 test failed: " #condition); \
} while (false)

namespace {
constexpr std::uint64_t kMiB = 1024ULL * 1024;

std::span<const std::byte> bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::string read_all(Pool& pool, const std::string& name) {
    const auto size = pool.Stat(name).size;
    std::string result(static_cast<std::size_t>(size), '\0');
    CHECK(pool.Read(name, 0,
        {reinterpret_cast<std::byte*>(result.data()), result.size()}) == result.size());
    return result;
}

void check_pattern(Pool& pool, std::uint64_t offset, std::size_t length) {
    std::vector<std::byte> actual(length);
    CHECK(pool.Read("spanning.bin", offset, actual) == length);
    for (std::size_t i = 0; i < length; ++i)
        CHECK(actual[i] == static_cast<std::byte>((offset + i) % 251));
}

template <typename F> void expect_error(poolfs::ErrorCode code, F&& action) {
    bool matched = false;
    try { action(); }
    catch (const poolfs::Error& error) { matched = error.code() == code; }
    CHECK(matched);
}
} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("poolfs-v3-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const auto a = dir / "a.img", b = dir / "b.img", c = dir / "c.img";
    const auto d = dir / "d.img", e = dir / "e.img";
    const std::vector<fs::path> three{a, b, c};
    const std::vector<fs::path> shuffled{c, a, b};
    const std::vector<fs::path> two{d, e};
    const auto cleanup = [&] {
        fs::remove(a); fs::remove(b); fs::remove(c);
        fs::remove(d); fs::remove(e);
        fs::remove(dir); // Never recursively remove an unexpected file.
    };
    try {
        Pool::FormatV3Images(three, Pool::kMinimumRawBytes);
        {
            Pool pool(Pool::V3ImageTag{}, shuffled);
            CHECK(pool.List().empty());
            CHECK(pool.Capacity() == 3 * (Pool::kMinimumRawBytes - 128 * kMiB));
            pool.Create("spanning.bin");
            const std::size_t size = static_cast<std::size_t>(129 * kMiB + 17);
            std::vector<std::byte> payload(size);
            for (std::size_t i = 0; i < size; ++i)
                payload[i] = static_cast<std::byte>(i % 251);
            CHECK(pool.Write("spanning.bin", 0, payload) == size);
            CHECK(pool.Stat("spanning.bin").size == size);
            check_pattern(pool, 0, 32);
            check_pattern(pool, 128 * kMiB - 16, 32);
            check_pattern(pool, size - 32, 32);
            pool.Create("note.txt");
            CHECK(pool.Write("note.txt", 0, bytes("hello world")) == 11);
            pool.Rename("note.txt", "renamed.txt");
            pool.Truncate("renamed.txt", 5);
            pool.Create("doomed.txt");
            pool.Remove("doomed.txt");
            pool.Create("last.txt");
        }
        {
            Pool pool(Pool::V3ImageTag{}, three);
            CHECK(!pool.RecoveredIncompleteTail());
            CHECK(pool.List().size() == 3);
            check_pattern(pool, 128 * kMiB - 16, 32);
            check_pattern(pool, 129 * kMiB - 15, 32);
            CHECK(read_all(pool, "renamed.txt") == "hello");
            CHECK(pool.Stat("last.txt").size == 0);
        }
        // One member's ninth journal slot is damaged. The common eight-record
        // prefix is readable, while v3 blocks further writes pending repair.
        {
            std::fstream image(c, std::ios::binary | std::ios::in | std::ios::out);
            CHECK(image.good());
            image.seekp(8192 + 8 * 4096);
            image.put('\0');
            image.flush();
            CHECK(image.good());
        }
        {
            Pool pool(Pool::V3ImageTag{}, three);
            CHECK(pool.RecoveredIncompleteTail());
            check_pattern(pool, 128 * kMiB - 16, 32);
            CHECK(read_all(pool, "renamed.txt") == "hello");
            expect_error(poolfs::ErrorCode::NotFound, [&] { pool.Stat("last.txt"); });
            expect_error(poolfs::ErrorCode::ReadOnly, [&] { pool.Create("blocked.txt"); });
        }
        expect_error(poolfs::ErrorCode::Corrupt, [&] {
            Pool incomplete(Pool::V3ImageTag{}, std::vector<fs::path>{a, b});
        });
        Pool::FormatV3Images(two, Pool::kMinimumRawBytes);
        {
            Pool pool(Pool::V3ImageTag{}, two);
            pool.Create("two-member.txt");
            CHECK(pool.Write("two-member.txt", 0, bytes("works")) == 5);
            CHECK(read_all(pool, "two-member.txt") == "works");
        }
        expect_error(poolfs::ErrorCode::Corrupt, [&] {
            Pool mixed(Pool::V3ImageTag{}, std::vector<fs::path>{a, b, e});
        });
        std::cout << "poolfs v3 image tests passed\n";
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
