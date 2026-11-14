#include "core.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace fs = std::filesystem;
using poolfs::Pool;

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error("test failed: " #condition); \
} while (false)

namespace {

std::span<const std::byte> bytes(const std::string& s) {
    return {reinterpret_cast<const std::byte*>(s.data()), s.size()};
}
std::string read_all(Pool& pool, const std::string& name) {
    const auto size = pool.Stat(name).size;
    std::string text(static_cast<std::size_t>(size), '\0');
    const auto n = pool.Read(name, 0,
        {reinterpret_cast<std::byte*>(text.data()), text.size()});
    CHECK(n == text.size());
    return text;
}
std::uint32_t read_u32(const std::array<unsigned char, 20>& b) {
    return std::uint32_t(b[4]) | std::uint32_t(b[5]) << 8 |
           std::uint32_t(b[6]) << 16 | std::uint32_t(b[7]) << 24;
}
std::pair<std::uint64_t, std::uint64_t> journal_end(const fs::path& image) {
    std::ifstream in(image, std::ios::binary);
    std::uint64_t pos = 8192;
    std::uint64_t next_sequence = 1;
    for (;;) {
        std::array<unsigned char, 20> h{};
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(h.data()), h.size());
        CHECK(in.good());
        if (h[0] == 0) return {pos, next_sequence};
        CHECK(h[0] == 'P' && h[1] == 'J' && h[2] == 'R' && h[3] == '1');
        pos += read_u32(h);
        ++next_sequence;
    }
}
void put32(std::vector<unsigned char>& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void put64(std::vector<unsigned char>& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
std::vector<unsigned char> extra_create_record(std::uint64_t sequence) {
    const std::string name = "uncommitted.txt";
    std::vector<unsigned char> payload;
    payload.push_back(1); // Create.
    payload.push_back(static_cast<unsigned char>(name.size()));
    payload.push_back(0);
    payload.insert(payload.end(), name.begin(), name.end());
    payload.push_back(0); // Member index.
    std::vector<unsigned char> record{'P','J','R','1'};
    put32(record, static_cast<std::uint32_t>(20 + payload.size()));
    put64(record, sequence);
    put32(record, 0);
    record.insert(record.end(), payload.begin(), payload.end());
    std::uint32_t crc = 2166136261U;
    for (auto c : record) { crc ^= c; crc *= 16777619U; }
    for (int i = 0; i < 4; ++i) record[16 + i] = static_cast<unsigned char>(crc >> (8 * i));
    return record;
}
template <typename F> void expect_error(poolfs::ErrorCode code, F&& fn) {
    bool caught = false;
    try { fn(); }
    catch (const poolfs::Error& e) { caught = e.code() == code; }
    CHECK(caught);
}

} // namespace

int main() {
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("poolfs-core-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const auto a = dir / "a.img", b = dir / "b.img";
    const auto c = dir / "c.img", d = dir / "d.img";
    const auto cleanup = [&] {
        fs::remove(a); fs::remove(b); fs::remove(c); fs::remove(d);
        fs::remove(dir); // Refuse to recurse if an unexpected file appeared.
    };
    try {
        Pool::Format(a, b, Pool::kMinimumImageBytes);
        expect_error(poolfs::ErrorCode::InvalidArgument,
            [&] { Pool::Format(a, b, Pool::kMinimumImageBytes); });
        {
            Pool pool(a, b);
            CHECK(pool.List().empty());
            CHECK(pool.Capacity() == 2 * (Pool::kMinimumImageBytes - 4 * 1024 * 1024));
            pool.Create("alpha.txt");
            CHECK(pool.Stat("alpha.txt").member == 0);
            CHECK(pool.Write("alpha.txt", 0, bytes("hello world")) == 11);
            pool.Create("beta.txt");
            CHECK(pool.Stat("beta.txt").member == 1);
            pool.Write("beta.txt", 0, bytes("second member"));
            CHECK(read_all(pool, "alpha.txt") == "hello world");
            pool.Write("alpha.txt", 6, bytes("pool"));
            CHECK(read_all(pool, "alpha.txt") == "hello poold");
            pool.Create("layered.bin");
            pool.Write("layered.bin", 0, bytes("abcdefgh"));
            pool.Write("layered.bin", 2, bytes("WXYZ"));
            pool.Write("layered.bin", 3, bytes("12"));
            pool.Write("layered.bin", 12, bytes("Q"));
            CHECK(read_all(pool, "layered.bin") ==
                  std::string("abW12Zgh\0\0\0\0Q", 13));
            std::array<std::byte, 5> window{};
            CHECK(pool.Read("layered.bin", 1, window) == window.size());
            CHECK(std::string(reinterpret_cast<const char*>(window.data()), window.size()) ==
                  "bW12Z");
            pool.Flush();
            pool.Truncate("alpha.txt", 5);
            pool.Truncate("alpha.txt", 8);
            CHECK(read_all(pool, "alpha.txt") == std::string("hello\0\0\0", 8));
            pool.Rename("alpha.txt", "renamed.txt");
            expect_error(poolfs::ErrorCode::NotFound, [&] { pool.Stat("alpha.txt"); });
            expect_error(poolfs::ErrorCode::AlreadyExists,
                [&] { pool.Create("renamed.txt"); });
            pool.Flush();
        }
        {
            Pool pool(b, a); // Pool members can be opened in either path order.
            CHECK(pool.List().size() == 3);
            CHECK(pool.Stat("renamed.txt").member == 0);
            CHECK(pool.Stat("beta.txt").member == 1);
            CHECK(read_all(pool, "renamed.txt") == std::string("hello\0\0\0", 8));
            CHECK(read_all(pool, "beta.txt") == "second member");
            CHECK(read_all(pool, "layered.bin") ==
                  std::string("abW12Zgh\0\0\0\0Q", 13));
            pool.Remove("layered.bin");
            pool.Remove("beta.txt");
            expect_error(poolfs::ErrorCode::NotFound, [&] { pool.Stat("beta.txt"); });
        }
        {
            Pool pool(a, b);
            CHECK(pool.List().size() == 1);
            CHECK(!pool.RecoveredIncompleteTail());
        }
        // A partial uncommitted journal tail is ignored on reopening.
        {
            const auto [tail, sequence] = journal_end(a);
            (void)sequence;
            std::fstream image(a, std::ios::binary | std::ios::in | std::ios::out);
            image.seekp(static_cast<std::streamoff>(tail));
            image.put(static_cast<char>(0xAA));
            image.flush();
        }
        {
            Pool pool(a, b);
            CHECK(pool.RecoveredIncompleteTail());
            CHECK(read_all(pool, "renamed.txt") == std::string("hello\0\0\0", 8));
            pool.Create("after-recovery.txt");
        }
        {
            Pool pool(a, b);
            CHECK(!pool.RecoveredIncompleteTail());
            CHECK(pool.Stat("after-recovery.txt").size == 0);
        }
        // A complete journal record written to one member only was never
        // acknowledged; the common committed prefix remains authoritative.
        {
            const auto [tail, sequence] = journal_end(a);
            auto record = extra_create_record(sequence);
            std::fstream image(a, std::ios::binary | std::ios::in | std::ios::out);
            image.seekp(static_cast<std::streamoff>(tail));
            image.write(reinterpret_cast<const char*>(record.data()),
                        static_cast<std::streamsize>(record.size()));
            image.flush();
        }
        {
            Pool pool(a, b);
            CHECK(pool.RecoveredIncompleteTail());
            expect_error(poolfs::ErrorCode::NotFound,
                [&] { pool.Stat("uncommitted.txt"); });
            CHECK(pool.Stat("after-recovery.txt").size == 0);
        }
        expect_error(poolfs::ErrorCode::Io, [&] { Pool missing(a, dir / "missing.img"); });
        Pool::Format(c, d, Pool::kMinimumImageBytes);
        expect_error(poolfs::ErrorCode::Corrupt, [&] { Pool mixed(a, d); });
        std::cout << "poolfs core tests passed\n";
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
