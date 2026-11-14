#include "core.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

namespace fs = std::filesystem;
using poolfs::Pool;

#define CHECK(condition) do { \
    if (!(condition)) throw std::runtime_error( \
        "v4 test failed at line " + std::to_string(__LINE__) + ": " #condition); \
} while (false)

namespace {
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::size_t kChunkBytes = 16 * static_cast<std::size_t>(kMiB);

std::span<const std::byte> bytes(const std::string& text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

void write_pattern(Pool& pool, const std::string& name, std::uint64_t size) {
    std::vector<std::byte> chunk(kChunkBytes);
    for (std::uint64_t offset = 0; offset < size;) {
        const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), size - offset));
        for (std::size_t i = 0; i < length; ++i)
            chunk[i] = static_cast<std::byte>((offset + i) % 251);
        CHECK(pool.Write(name, offset, {chunk.data(), length}) == length);
        offset += length;
    }
}

void check_pattern(Pool& pool, const std::string& name, std::uint64_t size) {
    std::vector<std::byte> chunk(static_cast<std::size_t>(kMiB));
    for (std::uint64_t offset = 0; offset < size;) {
        const auto length = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), size - offset));
        CHECK(pool.Read(name, offset, {chunk.data(), length}) == length);
        for (std::size_t i = 0; i < length; ++i)
            CHECK(chunk[i] == static_cast<std::byte>((offset + i) % 251));
        offset += length;
    }
}

void check_region(Pool& pool, const std::string& name,
                  std::uint64_t offset, std::byte expected) {
    std::array<std::byte, 128> actual{};
    CHECK(pool.Read(name, offset, actual) == actual.size());
    for (const auto value : actual) CHECK(value == expected);
}

void check_basic(const std::vector<fs::path>& members) {
    Pool::FormatV4Images(members, 512 * kMiB);
    constexpr auto large_size = 257 * kMiB + 17;
    std::uint64_t capacity = 0;
    std::uint64_t available_with_files = 0;
    {
        Pool pool(Pool::V4ImageTag{}, {members[1], members[0]});
        capacity = pool.Capacity();
        CHECK(capacity == 2 * (512 * kMiB - 256 * kMiB));
        CHECK(pool.Available() == capacity);

        pool.Create("large.bin");
        write_pattern(pool, "large.bin", large_size);
        CHECK(pool.Stat("large.bin").size == large_size);
        CHECK(pool.Available() < capacity - large_size);
        check_pattern(pool, "large.bin", large_size);

        pool.Create("partial.bin");
        std::vector<std::byte> original(16 * static_cast<std::size_t>(kMiB),
                                        std::byte{'A'});
        CHECK(pool.Write("partial.bin", 0, original) == original.size());
        const auto before_overwrite = pool.Available();
        std::vector<std::byte> replacement(static_cast<std::size_t>(kMiB),
                                           std::byte{'B'});
        CHECK(pool.Write("partial.bin", 4 * kMiB, replacement) == replacement.size());
        CHECK(pool.Available() == before_overwrite);
        check_region(pool, "partial.bin", 4 * kMiB - 128, std::byte{'A'});
        check_region(pool, "partial.bin", 4 * kMiB, std::byte{'B'});
        check_region(pool, "partial.bin", 5 * kMiB, std::byte{'A'});

        pool.Truncate("partial.bin", 8 * kMiB);
        CHECK(pool.Available() == before_overwrite + 8 * kMiB);
        CHECK(pool.Stat("partial.bin").size == 8 * kMiB);
        available_with_files = pool.Available();
    }
    {
        Pool pool(Pool::V4ImageTag{}, members);
        CHECK(pool.Available() == available_with_files);
        check_pattern(pool, "large.bin", large_size);
        check_region(pool, "partial.bin", 4 * kMiB - 128, std::byte{'A'});
        check_region(pool, "partial.bin", 4 * kMiB, std::byte{'B'});
        check_region(pool, "partial.bin", 5 * kMiB, std::byte{'A'});
        pool.Remove("large.bin");
        pool.Remove("partial.bin");
        CHECK(pool.Available() == capacity);

        pool.Create("reused.bin");
        write_pattern(pool, "reused.bin", large_size);
        check_pattern(pool, "reused.bin", large_size);
        pool.Remove("reused.bin");
        CHECK(pool.Available() == capacity);

        pool.Create("edge.bin");
        std::vector<std::byte> original(8192 + 17, std::byte{'C'});
        CHECK(pool.Write("edge.bin", 0, original) == original.size());
        const std::array<std::byte, 3> replacement{
            std::byte{'D'}, std::byte{'D'}, std::byte{'D'}};
        CHECK(pool.Write("edge.bin", 4095, replacement) == replacement.size());
        pool.Truncate("edge.bin", 4097);
        CHECK(pool.Stat("edge.bin").size == 4097);
    }
    {
        Pool pool(Pool::V4ImageTag{}, members);
        std::vector<std::byte> head(4097);
        CHECK(pool.Read("edge.bin", 0, head) == head.size());
        for (std::size_t i = 0; i < 4095; ++i) CHECK(head[i] == std::byte{'C'});
        CHECK(head[4095] == std::byte{'D'});
        CHECK(head[4096] == std::byte{'D'});

        pool.Truncate("edge.bin", 8192 + 17);
        std::vector<std::byte> gap(8192 + 17 - 4097);
        CHECK(pool.Read("edge.bin", 4097, gap) == gap.size());
        for (const auto value : gap) CHECK(value == std::byte{0});
        CHECK(pool.Write("edge.bin", 4097, bytes("again")) == 5);
        std::array<std::byte, 5> end{};
        CHECK(pool.Read("edge.bin", 4097, end) == end.size());
        CHECK(std::equal(end.begin(), end.end(), bytes("again").begin()));
        pool.Remove("edge.bin");
        CHECK(pool.Available() == capacity);
    }
}

void check_journal_rotation(const std::vector<fs::path>& members) {
    Pool::FormatV4Images(members, 512 * kMiB);
    constexpr std::uint64_t checkpoint_zero = 8192 + 64 * kMiB;
    std::array<std::array<char, 4096>, 2> old_headers{};
    for (std::size_t i = 0; i < members.size(); ++i) {
        std::ifstream member(members[i], std::ios::binary);
        CHECK(member.good());
        member.seekg(checkpoint_zero);
        member.read(old_headers[i].data(), old_headers[i].size());
        CHECK(member.good());
    }
    {
        Pool pool(Pool::V4ImageTag{}, members);
        const auto capacity = pool.Capacity();
        pool.Create("anchor.txt");
        CHECK(pool.Write("anchor.txt", 0, bytes("survives journal rotation")) == 25);
        // A 32 MiB epoch holds about 8,191 records. Cross the first boundary.
        for (std::size_t i = 0; i < 4100; ++i) {
            pool.Create("temporary.txt");
            pool.Remove("temporary.txt");
        }
        CHECK(pool.Available() <= capacity);
    }
    // Simulate a crash after publishing the second checkpoint but before
    // clearing the old checkpoint header. Recovery must choose the newer root.
    for (std::size_t i = 0; i < members.size(); ++i) {
        std::fstream member(members[i], std::ios::binary | std::ios::in | std::ios::out);
        CHECK(member.good());
        member.seekp(checkpoint_zero);
        member.write(old_headers[i].data(), old_headers[i].size());
        member.flush();
        CHECK(member.good());
    }
    {
        Pool pool(Pool::V4ImageTag{}, members);
        CHECK(pool.Stat("anchor.txt").size == 25);
        pool.Create("after_restore.txt");
        CHECK(pool.Write("after_restore.txt", 0, bytes("newer root")) == 10);
        // Cross the second boundary and reuse the first journal region.
        for (std::size_t i = 0; i < 4100; ++i) {
            pool.Create("temporary.txt");
            pool.Remove("temporary.txt");
        }
    }
    {
        Pool pool(Pool::V4ImageTag{}, {members[1], members[0]});
        CHECK(pool.Stat("anchor.txt").size == 25);
        std::array<std::byte, 25> actual{};
        CHECK(pool.Read("anchor.txt", 0, actual) == actual.size());
        CHECK(std::equal(actual.begin(), actual.end(),
            bytes("survives journal rotation").begin()));
        CHECK(pool.Stat("after_restore.txt").size == 10);
        CHECK(pool.List().size() == 2);
    }
}

void check_near_full_short_write(const std::vector<fs::path>& members) {
    Pool::FormatV4Images(members, 512 * kMiB);
    {
        Pool pool(Pool::V4ImageTag{}, members);
        const auto capacity = pool.Capacity();
        CHECK(capacity == 512 * kMiB);
        pool.Create("filler.bin");
        write_pattern(pool, "filler.bin", capacity - 4 * kMiB);
        CHECK(pool.Available() == 4 * kMiB);
        pool.Create("short.bin");
        std::vector<std::byte> payload(8 * static_cast<std::size_t>(kMiB),
                                       std::byte{'Z'});
        CHECK(pool.Write("short.bin", 0, payload) == 4 * kMiB);
        CHECK(pool.Stat("short.bin").size == 4 * kMiB);
        CHECK(pool.Available() == 0);
    }
    {
        Pool pool(Pool::V4ImageTag{}, {members[1], members[0]});
        std::vector<std::byte> data(4 * static_cast<std::size_t>(kMiB));
        CHECK(pool.Read("short.bin", 0, data) == data.size());
        for (const auto value : data) CHECK(value == std::byte{'Z'});
        bool no_space = false;
        try { pool.Write("short.bin", 4 * kMiB, bytes("x")); }
        catch (const poolfs::Error& error) {
            no_space = error.code() == poolfs::ErrorCode::NoSpace;
        }
        CHECK(no_space);
        pool.Remove("filler.bin");
        CHECK(pool.Available() == 508 * kMiB);
    }
}

void check_corrupt_member(const std::vector<fs::path>& members) {
    Pool::FormatV4Images(members, 512 * kMiB);
    {
        Pool pool(Pool::V4ImageTag{}, members);
        pool.Create("important.txt");
        CHECK(pool.Write("important.txt", 0, bytes("original data")) == 13);
    }
    // The second committed record is in epoch 0 at this fixed 4 KiB slot.
    // A member with a damaged record must not silently expose an old prefix.
    constexpr std::uint64_t record_byte = 8192 + 4096 + 32;
    {
        std::fstream member(members[0], std::ios::binary | std::ios::in | std::ios::out);
        CHECK(member.good());
        member.seekg(record_byte);
        char value = 0;
        member.get(value);
        CHECK(member.good());
        member.seekp(record_byte);
        member.put(static_cast<char>(value ^ 1));
        member.flush();
        CHECK(member.good());
    }
    bool rejected = false;
    try {
        Pool pool(Pool::V4ImageTag{}, members);
    } catch (const poolfs::Error& error) {
        rejected = error.code() == poolfs::ErrorCode::Corrupt;
    }
    CHECK(rejected);
}

void check_interrupted_last_commit(const std::vector<fs::path>& members) {
    Pool::FormatV4Images(members, 512 * kMiB);
    {
        Pool pool(Pool::V4ImageTag{}, members);
        pool.Create("anchor.txt");
        CHECK(pool.Write("anchor.txt", 0, bytes("original data")) == 13);
        pool.Create("uncommitted.txt");
    }
    // Erase only member 0's final committed record. Member 1 still has a
    // complete final record, so the two copies share a clean prefix.
    {
        std::fstream member(members[0], std::ios::binary | std::ios::in | std::ios::out);
        CHECK(member.good());
        const std::array<char, 4096> zero{};
        member.seekp(8192 + 2 * 4096);
        member.write(zero.data(), zero.size());
        member.flush();
        CHECK(member.good());
    }
    Pool recovered(Pool::V4ImageTag{}, members);
    CHECK(recovered.RecoveredIncompleteTail());
    CHECK(recovered.Stat("anchor.txt").size == 13);
    std::array<std::byte, 13> actual{};
    CHECK(recovered.Read("anchor.txt", 0, actual) == actual.size());
    CHECK(std::equal(actual.begin(), actual.end(), bytes("original data").begin()));
    bool missing = false;
    try { recovered.Stat("uncommitted.txt"); }
    catch (const poolfs::Error& error) {
        missing = error.code() == poolfs::ErrorCode::NotFound;
    }
    CHECK(missing);
    bool read_only = false;
    try { recovered.Create("blocked.txt"); }
    catch (const poolfs::Error& error) {
        read_only = error.code() == poolfs::ErrorCode::ReadOnly;
    }
    CHECK(read_only);
}

void check_differential(const std::vector<fs::path>& members) {
    Pool::FormatV4Images(members, 512 * kMiB);
    auto pool = std::make_unique<Pool>(Pool::V4ImageTag{}, members);
    const auto capacity = pool->Capacity();
    pool->Create("model.bin");
    std::vector<std::byte> expected;
    std::uint32_t state = 0x5eed1234;
    const auto next = [&] {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        return state;
    };
    const auto verify = [&] {
        CHECK(pool->Stat("model.bin").size == expected.size());
        std::vector<std::byte> actual(expected.size());
        CHECK(pool->Read("model.bin", 0, actual) == actual.size());
        CHECK(actual == expected);
        CHECK(pool->Available() <= capacity);
        CHECK(pool->Available() > capacity - 2 * kMiB);
    };
    for (std::size_t step = 0; step < 120; ++step) {
        if (step % 4 == 3) {
            const auto size = static_cast<std::size_t>(next() % (128 * 1024));
            pool->Truncate("model.bin", size);
            expected.resize(size, std::byte{0});
        } else {
            const auto offset = static_cast<std::size_t>(
                next() % (expected.size() + 4097));
            const auto length = static_cast<std::size_t>(1 + next() % 8191);
            std::vector<std::byte> payload(length);
            for (std::size_t i = 0; i < length; ++i)
                payload[i] = static_cast<std::byte>((step * 37 + i * 11) % 251);
            CHECK(pool->Write("model.bin", offset, payload) == length);
            if (offset + length > expected.size())
                expected.resize(offset + length, std::byte{0});
            std::copy(payload.begin(), payload.end(), expected.begin() + offset);
        }
        verify();
        if (step == 59) {
            const auto available = pool->Available();
            pool.reset();
            pool = std::make_unique<Pool>(Pool::V4ImageTag{},
                                          std::vector<fs::path>{members[1], members[0]});
            CHECK(pool->Available() == available);
            verify();
        }
    }
    const auto available = pool->Available();
    pool.reset();
    pool = std::make_unique<Pool>(Pool::V4ImageTag{}, members);
    CHECK(pool->Available() == available);
    verify();
    pool->Remove("model.bin");
    CHECK(pool->Available() == capacity);
}
} // namespace

int main(int argc, char** argv) {
    const bool stress = argc == 2 && std::string(argv[1]) == "--stress";
    if (argc > 2 || (argc == 2 && !stress))
        throw std::runtime_error("usage: poolfs-v4-tests.exe [--stress]");
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir = fs::temp_directory_path() / ("poolfs-v4-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const std::vector<fs::path> basic{dir / "basic-a.img", dir / "basic-b.img"};
    const std::vector<fs::path> rotation{dir / "rotation-a.img", dir / "rotation-b.img"};
    const std::vector<fs::path> corrupt{dir / "corrupt-a.img", dir / "corrupt-b.img"};
    const std::vector<fs::path> tail{dir / "tail-a.img", dir / "tail-b.img"};
    const std::vector<fs::path> model{dir / "model-a.img", dir / "model-b.img"};
    const std::vector<fs::path> full{dir / "full-a.img", dir / "full-b.img"};
    const auto cleanup = [&] {
        for (const auto& path : basic) fs::remove(path);
        for (const auto& path : rotation) fs::remove(path);
        for (const auto& path : corrupt) fs::remove(path);
        for (const auto& path : tail) fs::remove(path);
        for (const auto& path : model) fs::remove(path);
        for (const auto& path : full) fs::remove(path);
        fs::remove(dir); // Never recursively remove an unexpected file.
    };
    try {
        check_basic(basic);
        for (const auto& path : basic) fs::remove(path);
        check_corrupt_member(corrupt);
        for (const auto& path : corrupt) fs::remove(path);
        check_interrupted_last_commit(tail);
        for (const auto& path : tail) fs::remove(path);
        check_differential(model);
        for (const auto& path : model) fs::remove(path);
        if (stress) {
            check_journal_rotation(rotation);
            for (const auto& path : rotation) fs::remove(path);
            check_near_full_short_write(full);
            for (const auto& path : full) fs::remove(path);
        }
        std::cout << "poolfs v4 image tests passed"
                  << (stress ? " (including journal rotation)\n" : "\n");
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
