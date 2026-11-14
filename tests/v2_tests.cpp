#include "core.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
using poolfs::Pool;

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error("v2 test failed: " #condition); \
} while (false)

namespace {
std::span<const std::byte> bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}
std::string read_all(Pool& pool, const std::string& name) {
    const auto size = pool.Stat(name).size;
    std::string result(static_cast<std::size_t>(size), '\0');
    CHECK(pool.Read(name, 0, {reinterpret_cast<std::byte*>(result.data()), result.size()}) == result.size());
    return result;
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
    const fs::path dir = fs::temp_directory_path() / ("poolfs-v2-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const auto a = dir / "a.img", b = dir / "b.img";
    const auto c = dir / "c.img", d = dir / "d.img";
    const auto cleanup = [&] {
        fs::remove(a); fs::remove(b); fs::remove(c); fs::remove(d);
        fs::remove(dir); // Refuse to recurse if unexpected files appear.
    };
    try {
        Pool::FormatV2Images(a, b, Pool::kMinimumRawBytes);
        {
            Pool pool(Pool::V2ImageTag{}, a, b);
            CHECK(pool.List().empty());
            CHECK(pool.Capacity() == 2 * (Pool::kMinimumRawBytes - 128ULL * 1024 * 1024));
            CHECK(pool.Available() == pool.Capacity());
            pool.Create("alpha.txt");
            CHECK(pool.Write("alpha.txt", 0, bytes("hello")) == 5);
            CHECK(pool.Available() == pool.Capacity() - 4096);
            pool.Create("beta.txt");
            CHECK(pool.Write("beta.txt", 0, bytes("second")) == 6);
            CHECK(pool.Write("alpha.txt", 2, bytes("XYZ")) == 3);
            CHECK(pool.Available() == pool.Capacity() - 3 * 4096);
            CHECK(read_all(pool, "alpha.txt") == "heXYZ");
            CHECK(read_all(pool, "beta.txt") == "second");
            const std::string spanning(4097, 'L');
            pool.Create("spanning.bin");
            CHECK(pool.Write("spanning.bin", 0, bytes(spanning)) == spanning.size());
            CHECK(pool.Available() == pool.Capacity() - 5 * 4096);
            CHECK(read_all(pool, "spanning.bin") == spanning);
            pool.Truncate("beta.txt", 3);
            pool.Rename("alpha.txt", "renamed.txt");
            CHECK(read_all(pool, "renamed.txt") == "heXYZ");
        }
        {
            Pool pool(Pool::V2ImageTag{}, b, a);
            CHECK(!pool.RecoveredIncompleteTail());
            CHECK(read_all(pool, "renamed.txt") == "heXYZ");
            CHECK(read_all(pool, "beta.txt") == "sec");
            CHECK(read_all(pool, "spanning.bin") == std::string(4097, 'L'));
        }
        // Damage only the ninth mirrored record. Recovery must retain the
        // eight-record common prefix and drop the unacknowledgeable rename.
        {
            std::fstream image(a, std::ios::binary | std::ios::in | std::ios::out);
            CHECK(image.good());
            image.seekp(8192 + 8 * 4096);
            image.put('\0');
            image.flush();
            CHECK(image.good());
        }
        {
            Pool pool(Pool::V2ImageTag{}, a, b);
            CHECK(pool.RecoveredIncompleteTail());
            CHECK(read_all(pool, "alpha.txt") == "heXYZ");
            CHECK(read_all(pool, "beta.txt") == "sec");
            expect_error(poolfs::ErrorCode::NotFound, [&] { pool.Stat("renamed.txt"); });
            expect_error(poolfs::ErrorCode::ReadOnly, [&] { pool.Create("after-recovery.txt"); });
        }
        {
            Pool pool(Pool::V2ImageTag{}, a, b);
            CHECK(pool.RecoveredIncompleteTail());
            CHECK(read_all(pool, "alpha.txt") == "heXYZ");
        }
        Pool::FormatV2Images(c, d, Pool::kMinimumRawBytes);
        expect_error(poolfs::ErrorCode::Corrupt, [&] { Pool mixed(Pool::V2ImageTag{}, a, d); });
        expect_error(poolfs::ErrorCode::Corrupt, [&] { Pool legacy(a, b); });
        std::cout << "poolfs v2 image tests passed\n";
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
