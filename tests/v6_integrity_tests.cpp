#include "core.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using poolfs::Pool;

namespace {
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kMemberBytes = 512 * kMiB;
constexpr std::uint64_t kDataStart = 256 * kMiB;
constexpr std::uint64_t kBlockBytes = 4096;
constexpr std::uint64_t kBlocksPerChecksumPage = 512;
constexpr std::uint64_t kJournalEpochBytes = 32 * kMiB;
constexpr std::uint64_t kCheckpointStart = 2 * kBlockBytes + 2 * kJournalEpochBytes;
constexpr std::uint64_t kCheckpointSlotBytes =
    ((kDataStart - kCheckpointStart) / 2 / kBlockBytes) * kBlockBytes;
constexpr DWORD kTornPageExit = 87;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~UniqueHandle() {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
    }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    HANDLE get() const { return value_; }
    void reset(HANDLE value = INVALID_HANDLE_VALUE) {
        if (value_ && value_ != INVALID_HANDLE_VALUE) CloseHandle(value_);
        value_ = value;
    }
private:
    HANDLE value_;
};

struct ProcessResult {
    DWORD exit_code = 0;
    std::string output;
};

fs::path sibling_scrubber() {
    std::vector<wchar_t> module_path(32768);
    const auto length = GetModuleFileNameW(nullptr, module_path.data(),
                                           static_cast<DWORD>(module_path.size()));
    require(length && length < module_path.size(), "cannot locate v6 test executable");
    const fs::path executable(module_path.data(), module_path.data() + length);
    const auto scrubber = executable.parent_path() / L"poolfs-scrub.exe";
    require(fs::is_regular_file(scrubber), "poolfs-scrub.exe is missing beside v6 tests");
    return scrubber;
}

ProcessResult run_scrubber(const fs::path& scrubber,
                           const std::vector<fs::path>& members) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_raw = INVALID_HANDLE_VALUE;
    HANDLE write_raw = INVALID_HANDLE_VALUE;
    require(CreatePipe(&read_raw, &write_raw, &security, 0) != 0,
            "cannot create v6 scrub output pipe");
    UniqueHandle read_pipe(read_raw);
    UniqueHandle write_pipe(write_raw);
    require(SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0) != 0,
            "cannot make v6 scrub read pipe private");

    std::wstring command = L"\"" + scrubber.wstring() + L"\"";
    for (const auto& member : members) {
        const auto path = member.wstring();
        require(path.find(L'"') == std::wstring::npos,
                "unexpected quote in generated member path");
        command += L" \"" + path + L"\"";
    }
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = write_pipe.get();
    startup.hStdError = write_pipe.get();
    PROCESS_INFORMATION process_raw{};
    require(CreateProcessW(scrubber.c_str(), command.data(), nullptr, nullptr,
                           TRUE, CREATE_NO_WINDOW, nullptr, nullptr,
                           &startup, &process_raw) != 0,
            "cannot start poolfs-scrub.exe");
    UniqueHandle process(process_raw.hProcess);
    UniqueHandle thread(process_raw.hThread);
    write_pipe.reset();
    const auto wait = WaitForSingleObject(process.get(), 30000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.get(), 255);
        WaitForSingleObject(process.get(), INFINITE);
        throw std::runtime_error("poolfs-scrub.exe timed out");
    }
    require(wait == WAIT_OBJECT_0, "cannot wait for poolfs-scrub.exe");
    DWORD code = 0;
    require(GetExitCodeProcess(process.get(), &code) != 0,
            "cannot read poolfs-scrub.exe exit status");
    std::string output;
    std::array<char, 4096> buffer{};
    for (;;) {
        DWORD received = 0;
        if (!ReadFile(read_pipe.get(), buffer.data(),
                      static_cast<DWORD>(buffer.size()), &received, nullptr)) {
            require(GetLastError() == ERROR_BROKEN_PIPE,
                    "cannot read poolfs-scrub.exe output");
            break;
        }
        if (!received) break;
        output.append(buffer.data(), received);
    }
    return {code, output};
}

std::vector<std::byte> pattern(std::uint32_t seed) {
    std::vector<std::byte> bytes(8192);
    for (auto& value : bytes) {
        seed ^= seed << 13;
        seed ^= seed >> 17;
        seed ^= seed << 5;
        value = static_cast<std::byte>(seed & 0xff);
    }
    return bytes;
}

std::vector<std::byte> mixed_chunk(std::size_t index) {
    std::vector<std::byte> bytes(static_cast<std::size_t>(kMiB));
    auto state = 0x9E3779B9U ^
        (static_cast<std::uint32_t>(index) * 0x85EBCA6BU);
    for (auto& value : bytes) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        value = static_cast<std::byte>(state & 0xff);
    }
    return bytes;
}

struct Location {
    std::size_t member;
    std::uint64_t offset;
};

Location locate_data(const std::vector<fs::path>& members,
                     std::span<const std::byte> signature) {
    constexpr std::size_t kScanBytes = 4 * 1024 * 1024;
    require(signature.size() >= 64 && signature.size() < kScanBytes,
            "invalid data signature");
    std::vector<char> chunk(kScanBytes + signature.size() - 1);
    std::vector<char> needle(signature.size());
    for (std::size_t i = 0; i < signature.size(); ++i)
        needle[i] = static_cast<char>(signature[i]);
    for (std::size_t member = 0; member < members.size(); ++member) {
        std::ifstream input(members[member], std::ios::binary);
        require(input.good(), "cannot read v6 member image");
        std::uint64_t consumed = 0;
        std::size_t carry = 0;
        while (consumed < kMemberBytes) {
            const auto take = static_cast<std::streamsize>(
                std::min<std::uint64_t>(kScanBytes, kMemberBytes - consumed));
            input.read(chunk.data() + carry, take);
            require(input.gcount() == take, "short v6 member image scan");
            const auto end = chunk.begin() + carry + static_cast<std::size_t>(take);
            const auto found = std::search(chunk.begin(), end,
                                           needle.begin(), needle.end());
            if (found != end)
                return {member, consumed - carry + static_cast<std::uint64_t>(
                    std::distance(chunk.begin(), found))};
            carry = std::min(signature.size() - 1,
                             carry + static_cast<std::size_t>(take));
            std::copy(end - carry, end, chunk.begin());
            consumed += static_cast<std::uint64_t>(take);
        }
    }
    throw std::runtime_error("committed v6 data signature was not found");
}

void flip_byte(const fs::path& path, std::uint64_t offset) {
    std::fstream image(path, std::ios::binary | std::ios::in | std::ios::out);
    require(image.good(), "cannot open v6 image for byte flip");
    image.seekg(static_cast<std::streamoff>(offset));
    char original = 0;
    image.get(original);
    require(image.good(), "cannot read v6 image byte");
    image.seekp(static_cast<std::streamoff>(offset));
    image.put(static_cast<char>(original ^ 1));
    image.flush();
    require(image.good(), "cannot write v6 image byte");
}

std::array<std::byte, 4096> read_page(const fs::path& path, std::uint64_t offset) {
    std::array<std::byte, 4096> page{};
    std::ifstream image(path, std::ios::binary);
    require(image.good(), "cannot open v6 image for checksum-page comparison");
    image.seekg(static_cast<std::streamoff>(offset));
    image.read(reinterpret_cast<char*>(page.data()),
               static_cast<std::streamsize>(page.size()));
    require(image.gcount() == static_cast<std::streamsize>(page.size()),
            "short v6 checksum-page read");
    return page;
}

struct CheckpointLocation {
    std::uint64_t generation = 0;
    std::uint8_t epoch = 0;
};

CheckpointLocation selected_checkpoint(const fs::path& path) {
    CheckpointLocation selected;
    constexpr std::array<char, 8> magic{'P','F','S','C','P','0','0','6'};
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
        const auto page = read_page(path, kCheckpointStart + slot * kCheckpointSlotBytes);
        bool matches = true;
        for (std::size_t i = 0; i < magic.size(); ++i)
            matches &= page[i] == static_cast<std::byte>(magic[i]);
        if (!matches) continue;
        std::uint64_t generation = 0;
        for (int i = 0; i < 8; ++i)
            generation |= std::uint64_t(std::to_integer<std::uint8_t>(page[8 + i])) << (8 * i);
        if (generation > selected.generation)
            selected = {generation, std::to_integer<std::uint8_t>(page[24])};
    }
    require(selected.generation != 0 && selected.epoch < 2,
            "cannot find selected v6 checkpoint");
    return selected;
}

void clear_journal_epoch(const std::vector<fs::path>& members, std::uint8_t epoch) {
    const std::array<char, 4096> zero{};
    for (const auto& member : members) {
        std::fstream image(member, std::ios::binary | std::ios::in | std::ios::out);
        require(image.good(), "cannot open fixture for journal discard");
        image.seekp(static_cast<std::streamoff>(2 * kBlockBytes +
                                                epoch * kJournalEpochBytes));
        image.write(zero.data(), static_cast<std::streamsize>(zero.size()));
        image.flush();
        require(image.good(), "cannot discard fixture journal epoch");
    }
}

void set_earlier_layout(const fs::path& member, std::uint8_t revision) {
    std::fstream image(member, std::ios::binary | std::ios::in | std::ios::out);
    require(image.good(), "cannot open fixture for v6 layout rewrite");
    for (const std::uint64_t offset : {std::uint64_t{0}, kBlockBytes}) {
        auto header = read_page(member, offset);
        header[46] = static_cast<std::byte>(revision);
        std::fill_n(header.begin() + 56, 4, std::byte{0});
        std::uint32_t hash = 2166136261U;
        for (const auto value : header) {
            hash ^= std::to_integer<std::uint8_t>(value);
            hash *= 16777619U;
        }
        for (int i = 0; i < 4; ++i)
            header[56 + i] = static_cast<std::byte>(hash >> (8 * i));
        image.seekp(static_cast<std::streamoff>(offset));
        image.write(reinterpret_cast<const char*>(header.data()),
                    static_cast<std::streamsize>(header.size()));
    }
    image.flush();
    require(image.good(), "cannot rewrite fixture v6 layout headers");
}

std::array<std::uint64_t, 2> checksum_slots_for(std::uint64_t data_offset) {
    require(data_offset >= kDataStart && data_offset < kMemberBytes,
            "located v6 payload is outside the data region");
    const auto available = kMemberBytes - kDataStart;
    auto blocks = available / (kBlockBytes + 16);
    auto groups_for = [](std::uint64_t count) {
        return (count + kBlocksPerChecksumPage - 1) / kBlocksPerChecksumPage;
    };
    while (blocks * kBlockBytes + 2 * groups_for(blocks) * kBlockBytes > available)
        --blocks;
    const auto groups = groups_for(blocks);
    const auto table_start = kMemberBytes - 2 * groups * kBlockBytes;
    const auto block_index = (data_offset - kDataStart) / kBlockBytes;
    require(block_index < blocks, "located payload is outside v6 data blocks");
    const auto group = block_index / kBlocksPerChecksumPage;
    return {table_start + 2 * group * kBlockBytes,
            table_start + (2 * group + 1) * kBlockBytes};
}

DWORD run_torn_write_child(const std::vector<fs::path>& members) {
    std::vector<wchar_t> module_path(32768);
    const auto length = GetModuleFileNameW(nullptr, module_path.data(),
                                           static_cast<DWORD>(module_path.size()));
    require(length && length < module_path.size(), "cannot locate v6 test child");
    const fs::path executable(module_path.data(), module_path.data() + length);
    std::wstring command = L"\"" + executable.wstring() + L"\" --tear-child";
    for (const auto& member : members) {
        const auto path = member.wstring();
        require(path.find(L'"') == std::wstring::npos,
                "unexpected quote in generated v6 member path");
        command += L" \"" + path + L"\"";
    }
    require(SetEnvironmentVariableW(L"POOLFS_TEST_V6_TEAR_CHECKSUM_PAGE", L"1") != 0,
            "cannot enable v6 torn-page test hook");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_raw{};
    const BOOL started = CreateProcessW(executable.c_str(), command.data(),
        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process_raw);
    SetEnvironmentVariableW(L"POOLFS_TEST_V6_TEAR_CHECKSUM_PAGE", nullptr);
    require(started != 0, "cannot start v6 torn-page child");
    UniqueHandle process(process_raw.hProcess);
    UniqueHandle thread(process_raw.hThread);
    const auto wait = WaitForSingleObject(process.get(), 30000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.get(), 255);
        WaitForSingleObject(process.get(), INFINITE);
        throw std::runtime_error("v6 torn-page child timed out");
    }
    require(wait == WAIT_OBJECT_0, "cannot wait for v6 torn-page child");
    DWORD code = 0;
    require(GetExitCodeProcess(process.get(), &code) != 0,
            "cannot read v6 torn-page child exit status");
    return code;
}

void run_checkpoint_child(const std::vector<fs::path>& members,
                          std::wstring_view mode) {
    std::vector<wchar_t> module_path(32768);
    const auto length = GetModuleFileNameW(nullptr, module_path.data(),
                                           static_cast<DWORD>(module_path.size()));
    require(length && length < module_path.size(), "cannot locate checkpoint test child");
    const fs::path executable(module_path.data(), module_path.data() + length);
    std::wstring command = L"\"" + executable.wstring() + L"\" " +
                           std::wstring(mode);
    for (const auto& member : members) {
        const auto path = member.wstring();
        require(path.find(L'"') == std::wstring::npos,
                "unexpected quote in checkpoint member path");
        command += L" \"" + path + L"\"";
    }
    require(SetEnvironmentVariableW(L"POOLFS_TEST_V6_CHECKPOINT_AFTER_WRITE", L"1") != 0,
            "cannot enable checkpoint test hook");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process_raw{};
    const BOOL started = CreateProcessW(executable.c_str(), command.data(),
        nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr,
        &startup, &process_raw);
    SetEnvironmentVariableW(L"POOLFS_TEST_V6_CHECKPOINT_AFTER_WRITE", nullptr);
    require(started != 0, "cannot start checkpoint test child");
    UniqueHandle process(process_raw.hProcess);
    UniqueHandle thread(process_raw.hThread);
    const auto wait = WaitForSingleObject(process.get(), 30000);
    if (wait == WAIT_TIMEOUT) {
        TerminateProcess(process.get(), 255);
        WaitForSingleObject(process.get(), INFINITE);
        throw std::runtime_error("checkpoint test child timed out");
    }
    require(wait == WAIT_OBJECT_0, "cannot wait for checkpoint test child");
    DWORD code = 0;
    require(GetExitCodeProcess(process.get(), &code) != 0 && code == 0,
            "checkpoint test child failed");
}

void check_normal_io(const fs::path& scrubber,
                     const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    auto expected = pattern(0x1234567U);
    {
        Pool pool(Pool::V6ImageTag{}, members);
        require(pool.Capacity() >= 400 * kMiB,
                "unexpectedly small v6 data capacity");
        pool.Create("target.bin");
        require(pool.Write("target.bin", 0, expected) == expected.size(),
                "initial v6 write was short");
        const std::array<std::byte, 3> patch{
            std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}};
        require(pool.Write("target.bin", 4095, patch) == patch.size(),
                "cross-block v6 overwrite was short");
        std::copy(patch.begin(), patch.end(), expected.begin() + 4095);
        for (std::size_t iteration = 0; iteration < 6; ++iteration) {
            const auto position = static_cast<std::uint64_t>(512 + iteration * 997);
            const std::array<std::byte, 1> replacement{
                static_cast<std::byte>('a' + iteration)};
            require(pool.Write("target.bin", position, replacement) == 1,
                    "repeated v6 checksum-root update was short");
            expected[static_cast<std::size_t>(position)] = replacement[0];
            std::vector<std::byte> actual(expected.size());
            require(pool.Read("target.bin", 0, actual) == actual.size() &&
                        actual == expected,
                    "repeated v6 checksum-root update changed file bytes");
        }
        require(pool.Available() == pool.Capacity() - expected.size(),
                "v6 repeated copy-on-write updates leaked data blocks");

        const auto before_reclaim = pool.Available();
        pool.Create("reclaim.bin");
        const auto reclaimed_data = pattern(0xA5A5A5A5U);
        require(pool.Write("reclaim.bin", 0, reclaimed_data) == reclaimed_data.size(),
                "reclaim fixture write was short");
        pool.Remove("reclaim.bin");
        require(pool.Available() == before_reclaim,
                "removing a v6 file did not reclaim data and checksum pages");
        const auto busy = run_scrubber(scrubber, members);
        require(busy.exit_code == 1 &&
                    busy.output.find("status: ERROR") != std::string::npos,
                "v6 scrub did not refuse in-use member images");
    }
    {
        Pool pool(Pool::V6ImageTag{}, {members[1], members[0]});
        std::vector<std::byte> actual(expected.size());
        require(pool.Read("target.bin", 0, actual) == actual.size() && actual == expected,
                "v6 data changed after reversed-path reopen");
        pool.Truncate("target.bin", 4100);
        pool.Truncate("target.bin", 8192);
    }
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> actual(8192);
        require(pool.Read("target.bin", 0, actual) == actual.size(),
                "v6 read after regrow was short");
        require(std::equal(actual.begin(), actual.begin() + 4100, expected.begin()),
                "v6 truncation changed retained bytes");
        require(std::all_of(actual.begin() + 4100, actual.end(),
                    [](std::byte b) { return b == std::byte{0}; }),
                "v6 regrown range is not zero");
    }
    {
        Pool pool(Pool::V6ReadOnlyImageTag{}, members);
        require(pool.Scrub({}) == 0, "v6 scrub rejected clean data");
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "v6 scrub CLI did not report a clean pool");
}

void check_mixed_size_io(const fs::path& scrubber,
                         const std::vector<fs::path>& members) {
    constexpr std::uint64_t kSmallBytes = 384 * kMiB;
    constexpr std::uint64_t kLargeBytes = 512 * kMiB;
    constexpr std::uint64_t kExpectedCapacity = 401080320;
    constexpr std::size_t kFileMiB = 300;
    require(members.size() == 2, "mixed-size fixture requires two members");
    bool rejected_small_member = false;
    try {
        Pool::FormatV6Images(std::vector<poolfs::ImageMemberSpec>{
            {members[0], kSmallBytes}, {members[1], 383 * kMiB}});
    } catch (const poolfs::Error& error) {
        rejected_small_member = error.code() == poolfs::ErrorCode::InvalidArgument;
    }
    require(rejected_small_member && !fs::exists(members[0]) &&
                !fs::exists(members[1]),
            "mixed-size v6 formatter accepted an undersized member or created a partial pool");
    Pool::FormatV6Images(std::vector<poolfs::ImageMemberSpec>{
        {members[0], kSmallBytes}, {members[1], kLargeBytes}});
    require(fs::file_size(members[0]) == kSmallBytes &&
                fs::file_size(members[1]) == kLargeBytes,
            "mixed-size formatter did not create the requested member lengths");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        require(pool.Capacity() == kExpectedCapacity &&
                    pool.Available() == kExpectedCapacity,
                "mixed-size v6 capacity did not sum individual member regions");
        pool.Create("spanning.bin");
        for (std::size_t index = 0; index < kFileMiB; ++index) {
            const auto chunk = mixed_chunk(index);
            require(pool.Write("spanning.bin",
                               static_cast<std::uint64_t>(index) * kMiB,
                               chunk) == chunk.size(),
                    "mixed-size v6 file write was short");
        }
        require(pool.Stat("spanning.bin").size == kFileMiB * kMiB &&
                    pool.Available() == kExpectedCapacity - kFileMiB * kMiB,
                "mixed-size v6 file did not occupy the expected pooled capacity");
    }
    {
        Pool pool(Pool::V6ImageTag{}, {members[1], members[0]});
        require(pool.Capacity() == kExpectedCapacity &&
                    pool.Stat("spanning.bin").size == kFileMiB * kMiB,
                "mixed-size v6 metadata changed after reversed-path reopen");
        std::vector<std::byte> actual(static_cast<std::size_t>(kMiB));
        for (std::size_t index = 0; index < kFileMiB; ++index) {
            const auto expected = mixed_chunk(index);
            require(pool.Read("spanning.bin",
                              static_cast<std::uint64_t>(index) * kMiB,
                              actual) == actual.size() && actual == expected,
                    "mixed-size v6 file bytes changed after reopen");
        }
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("1 file(s) scanned") != std::string::npos &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "mixed-size v6 cross-member file did not pass offline scrub");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Remove("spanning.bin");
        require(pool.Available() == kExpectedCapacity,
                "mixed-size v6 pool did not reclaim the spanning file");
    }
}

void check_corrupt_data(const fs::path& scrubber,
                        const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    const auto healthy = pattern(0xDEADBEEFU);
    const auto affected = pattern(0xBADC0DEU);
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Create("healthy.bin");
        pool.Create("affected.bin");
        require(pool.Write("healthy.bin", 0, healthy) == healthy.size(),
                "healthy fixture write was short");
        require(pool.Write("affected.bin", 0, affected) == affected.size(),
                "affected fixture write was short");
    }
    const auto location = locate_data(members,
        std::span<const std::byte>(affected).subspan(128, 64));
    const auto healthy_location = locate_data(members,
        std::span<const std::byte>(healthy).subspan(128, 64));
    require(healthy_location.member != location.member,
            "v6 corruption fixtures did not use separate members");
    flip_byte(members[location.member], location.offset + 16);
    const auto damaged = run_scrubber(scrubber, members);
    require(damaged.exit_code == 2 &&
                damaged.output.find("status: DAMAGED") != std::string::npos &&
                damaged.output.find("file=affected.bin") != std::string::npos &&
                damaged.output.find("member=" + std::to_string(location.member)) !=
                    std::string::npos,
            "v6 scrub CLI did not report damaged file/member");
    {
        Pool pool(Pool::V6ReadOnlyImageTag{}, members);
        std::vector<poolfs::ScrubIssue> issues;
        const auto count = pool.Scrub([&](const poolfs::ScrubIssue& issue) {
            issues.push_back(issue);
        });
        require(count == 1 && issues.size() == 1 &&
                    issues[0].name == "affected.bin" &&
                    issues[0].member == location.member,
                "v6 scrub did not identify the damaged file/member");
    }
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> readback(healthy.size());
        require(pool.Read("healthy.bin", 0, readback) == healthy.size() &&
                    readback == healthy,
                "healthy v6 file became unreadable after another file was damaged");
        bool rejected = false;
        try {
            std::array<std::byte, 16> bytes{};
            pool.Read("affected.bin", 144, bytes);
        } catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::Corrupt;
        }
        require(rejected, "v6 read returned a damaged data block");
    }
    flip_byte(members[location.member], location.offset + 16);
    {
        Pool pool(Pool::V6ReadOnlyImageTag{}, members);
        require(pool.Scrub({}) == 0, "restored v6 data did not pass scrub");
    }

    // The first write in this physical group publishes slot 0. Damage its
    // referenced checksum page, leaving the other member's file readable.
    const auto pages = checksum_slots_for(location.offset);
    flip_byte(members[location.member], pages[0] + 16);
    const auto damaged_page = run_scrubber(scrubber, members);
    require(damaged_page.exit_code == 2 &&
                damaged_page.output.find("status: DAMAGED") != std::string::npos &&
                damaged_page.output.find("file=affected.bin") != std::string::npos,
            "v6 scrub CLI missed a damaged checksum page");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> readback(healthy.size());
        require(pool.Read("healthy.bin", 0, readback) == healthy.size() &&
                    readback == healthy,
                "checksum-page damage made an unrelated file unreadable");
        bool rejected = false;
        try {
            std::array<std::byte, 16> bytes{};
            pool.Read("affected.bin", 144, bytes);
        } catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::Corrupt;
        }
        require(rejected, "v6 read returned data with a damaged checksum page");
    }
}

void check_torn_checksum_page(const fs::path& scrubber,
                              const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    const auto anchor = pattern(0x13579BDFU);
    auto acknowledged = pattern(0x2468ACE0U);
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Create("anchor.bin");
        pool.Create("target.bin");
        require(pool.Write("anchor.bin", 0, anchor) == anchor.size(),
                "cannot seed acknowledged v6 anchor");
        require(pool.Write("target.bin", 0, acknowledged) == acknowledged.size(),
                "cannot seed v6 torn-page target");
        // Small COW rewrites alternate between the two members' first data
        // groups. Four acknowledged rewrites toggle each group's checksum
        // root from slot 0 to 1 and back to slot 0 before the torn write.
        for (std::size_t iteration = 0; iteration < 4; ++iteration) {
            const auto value = static_cast<std::byte>('C' + iteration);
            const std::array<std::byte, 3> patch{value, value, value};
            require(pool.Write("target.bin", 4095, patch) == patch.size(),
                    "cannot seed acknowledged v6 checksum-root toggles");
            std::copy(patch.begin(), patch.end(), acknowledged.begin() + 4095);
            std::vector<std::byte> readback(acknowledged.size());
            require(pool.Read("target.bin", 0, readback) == readback.size() &&
                        readback == acknowledged,
                    "acknowledged v6 root toggle changed target bytes");
        }
    }
    const auto location = locate_data(members,
        std::span<const std::byte>(acknowledged).subspan(4064, 64));
    const auto pages = checksum_slots_for(location.offset);
    const auto original_page = read_page(members[location.member], pages[0]);
    const auto group_zero_pages = checksum_slots_for(kDataStart);
    std::array<std::array<std::byte, 4096>, 2> old_active_pages{};
    std::array<std::array<std::byte, 4096>, 2> old_inactive_pages{};
    for (std::size_t member = 0; member < members.size(); ++member) {
        old_active_pages[member] = read_page(members[member], group_zero_pages[0]);
        old_inactive_pages[member] = read_page(members[member], group_zero_pages[1]);
    }

    require(run_torn_write_child(members) == kTornPageExit,
            "v6 checksum-page tear hook did not interrupt the child");
    require(read_page(members[location.member], pages[0]) == original_page,
            "torn write modified a checksum page still referenced by the old state");
    bool inactive_page_changed = false;
    for (std::size_t member = 0; member < members.size(); ++member) {
        require(read_page(members[member], group_zero_pages[0]) == old_active_pages[member],
                "torn write changed an active checksum page");
        inactive_page_changed |= read_page(members[member], group_zero_pages[1]) !=
                                 old_inactive_pages[member];
    }
    require(inactive_page_changed,
            "torn-page hook did not persist a partial inactive checksum page");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> actual(acknowledged.size());
        require(pool.Read("target.bin", 0, actual) == actual.size() &&
                    actual == acknowledged,
                "v6 torn checksum-page write changed unacknowledged target data");
        actual.resize(anchor.size());
        require(pool.Read("anchor.bin", 0, actual) == actual.size() &&
                    actual == anchor,
                "v6 torn checksum-page write changed acknowledged anchor");
        require(!pool.RecoveredIncompleteTail(),
                "torn checksum page reached a journal commit");
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "v6 torn checksum page made old live data fail scrub");
}

void check_stale_root_reuse(const fs::path& scrubber,
                            const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    const auto obsolete = pattern(0x719E42B5U);
    const auto replacement = pattern(0xA31F086DU);
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Create("obsolete.bin");
        require(pool.Write("obsolete.bin", 0, obsolete) == obsolete.size(),
                "cannot seed obsolete v6 checksum group");
        pool.Remove("obsolete.bin");
        require(pool.Available() == pool.Capacity(),
                "deleting obsolete file did not release its v6 data blocks");
    }
    const auto old_location = locate_data(members,
        std::span<const std::byte>(obsolete).subspan(128, 64));
    const auto pages = checksum_slots_for(old_location.offset);
    // This page is no longer referenced by any live extent. New writes must
    // be able to reset the group rather than requiring its obsolete hash.
    flip_byte(members[old_location.member], pages[0] + 16);
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Create("replacement.bin");
        require(pool.Write("replacement.bin", 0, replacement) == replacement.size(),
                "damaged obsolete v6 checksum page prevented freed-block reuse");
        std::vector<std::byte> actual(replacement.size());
        require(pool.Read("replacement.bin", 0, actual) == actual.size() &&
                    actual == replacement,
                "reused v6 group returned wrong replacement bytes");
        require(pool.Available() == pool.Capacity() - replacement.size(),
                "reusing a freed v6 group leaked data blocks");
    }
    const auto new_location = locate_data(members,
        std::span<const std::byte>(replacement).subspan(128, 64));
    require(new_location.member == old_location.member &&
                new_location.offset == old_location.offset,
            "stale-root fixture did not reuse the same physical data blocks");
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "reused v6 checksum group did not pass offline scrub");
}

void check_checkpoint_retention(const fs::path& scrubber,
                                const std::vector<fs::path>& data_members,
                                const std::vector<fs::path>& page_members) {
    const auto original = pattern(0x1BADB002U);
    const auto replacement = pattern(0xED95A711U);
    Pool::FormatV6Images(data_members, kMemberBytes);
    run_checkpoint_child(data_members, L"--checkpoint-child");
    const auto original_cp = selected_checkpoint(data_members[0]);
    require(original_cp.generation == 2,
            "checkpoint fixture did not publish the seeded file");
    {
        Pool pool(Pool::V6ImageTag{}, data_members);
        pool.Remove("snapshot.bin");
        require(pool.Available() == pool.Capacity() - original.size(),
                "deleted checkpoint data was made reusable too early");
        pool.Create("replacement.bin");
        require(pool.Write("replacement.bin", 0, replacement) == replacement.size(),
                "cannot write replacement after checkpoint deletion");
    }
    require(selected_checkpoint(data_members[0]).generation == 2,
            "checkpoint changed without a page-slot conflict");
    // Deliberately discard post-checkpoint journal records in this disposable
    // fixture. The selected checkpoint must still name intact data and pages.
    clear_journal_epoch(data_members, original_cp.epoch);
    {
        Pool pool(Pool::V6ImageTag{}, data_members);
        std::vector<std::byte> actual(original.size());
        require(pool.Read("snapshot.bin", 0, actual) == original.size() &&
                    actual == original,
                "checkpoint data was overwritten after the file was deleted");
    }
    const auto data_clean = run_scrubber(scrubber, data_members);
    require(data_clean.exit_code == 0 &&
                data_clean.output.find("status: CLEAN") != std::string::npos,
            "checkpoint data failed scrub after journal discard");

    Pool::FormatV6Images(page_members, kMemberBytes);
    run_checkpoint_child(page_members, L"--checkpoint-child");
    auto checkpoint_bytes = original;
    CheckpointLocation protected_cp;
    for (std::uint32_t iteration = 0; iteration < 8; ++iteration) {
        const auto next = pattern(0xA377B100U + iteration);
        {
            Pool pool(Pool::V6ImageTag{}, page_members);
            require(pool.Write("snapshot.bin", 0, next) == next.size(),
                    "cannot write checksum-page replacement");
        }
        const auto observed = selected_checkpoint(page_members[0]);
        if (iteration == 0)
            require(observed.generation == 2,
                    "first checksum-page update rotated an unconflicted checkpoint");
        if (observed.generation > 2) {
            protected_cp = observed;
            break;
        }
        checkpoint_bytes = next;
    }
    require(protected_cp.generation == 3,
            "page-slot reuse did not first publish a new checkpoint");
    clear_journal_epoch(page_members, protected_cp.epoch);
    {
        Pool pool(Pool::V6ImageTag{}, page_members);
        std::vector<std::byte> actual(checkpoint_bytes.size());
        require(pool.Read("snapshot.bin", 0, actual) == checkpoint_bytes.size() &&
                    actual == checkpoint_bytes,
                "checkpoint checksum page was overwritten after a second update");
    }
    const auto page_clean = run_scrubber(scrubber, page_members);
    require(page_clean.exit_code == 0 &&
                page_clean.output.find("status: CLEAN") != std::string::npos,
            "checkpoint checksum page failed scrub after journal discard");
}

void check_checkpoint_space_recovery(const fs::path& scrubber,
                                     const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, 384 * kMiB);
    constexpr std::size_t kBulkMiB = 200;
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Create("bulk.bin");
        for (std::size_t i = 0; i < kBulkMiB; ++i) {
            const auto chunk = mixed_chunk(i);
            require(pool.Write("bulk.bin", i * kMiB, chunk) == chunk.size(),
                    "cannot seed near-full checkpoint fixture");
        }
    }
    run_checkpoint_child(members, L"--checkpoint-rotate-child");
    require(selected_checkpoint(members[0]).generation == 2,
            "near-full fixture did not publish a checkpoint");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.Remove("bulk.bin");
        require(pool.Available() < kBulkMiB * kMiB,
                "deleted checkpoint extents were made available too early");
        pool.Create("replacement.bin");
        for (std::size_t i = 0; i < kBulkMiB; ++i) {
            const auto chunk = mixed_chunk(i + 1000);
            require(pool.Write("replacement.bin", i * kMiB, chunk) == chunk.size(),
                    "checkpoint rotation did not reclaim retained near-full space");
        }
        require(pool.Stat("replacement.bin").size == kBulkMiB * kMiB,
                "near-full replacement has the wrong length");
    }
    require(selected_checkpoint(members[0]).generation > 2,
            "retained checkpoint space was reused without publishing a new root");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> actual(static_cast<std::size_t>(kMiB));
        for (std::size_t i : {std::size_t{0}, kBulkMiB / 2, kBulkMiB - 1}) {
            const auto expected = mixed_chunk(i + 1000);
            require(pool.Read("replacement.bin", i * kMiB, actual) == actual.size() &&
                        actual == expected,
                    "near-full replacement changed after reopen");
        }
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "near-full checkpoint rotation did not pass scrub");
}

void check_old_layout_rejection(const fs::path& scrubber,
                                const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    const auto checker = scrubber.parent_path() / L"poolfs-check.exe";
    require(fs::is_regular_file(checker), "poolfs-check.exe is missing beside v6 tests");
    for (const std::uint8_t revision : {std::uint8_t{0}, std::uint8_t{1}}) {
        for (const auto& member : members) set_earlier_layout(member, revision);
        bool rejected = false;
        try {
            Pool pool(Pool::V6ImageTag{}, members);
        } catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::Corrupt &&
                std::string_view(error.what()).find("unsupported earlier v6") !=
                    std::string_view::npos;
        }
        require(rejected, "earlier v6 layout was accepted by the mount core");
        const auto report = run_scrubber(checker, members);
        require(report.exit_code == 3 &&
                    report.output.find("unsupported earlier v6 layout") !=
                        std::string::npos,
                "earlier v6 layout was accepted by the offline checker");
    }
}

void check_namespace_metadata(const fs::path& scrubber,
                              const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    std::uint64_t file_id = 0;
    std::uint64_t directory_id = 0;
    const std::string long_first(135, 'a');
    const std::string long_second(135, 'b');
    const std::string long_path = long_first + '/' + long_second + "/file.txt";
    std::string unicode_name;
    for (int i = 0; i < 100; ++i) unicode_name += "\xE9\x9B\xAA"; // U+96EA
    {
        Pool pool(Pool::V6ImageTag{}, members);
        require(pool.Root().id == 1 && pool.Root().is_directory,
                "v6 root has no stable directory identity");
        pool.MakeDirectory("Folder");
        pool.MakeDirectory("NormalDir", 0x80);
        require(pool.Stat("NormalDir").attributes == 0x10,
                "directory FILE_ATTRIBUTE_NORMAL did not normalize");
        pool.SetBasicInfo(pool.Stat("NormalDir").id, 0x80, 0, 0, 0, 0);
        require(pool.Stat("NormalDir").attributes == 0x10,
                "directory SetBasicInfo NORMAL did not normalize");
        pool.MakeDirectory("Folder/Sub");
        directory_id = pool.Stat("Folder").id;
        pool.SetBasicInfo(directory_id, 0x12, 132000000200000000ULL, 0, 0, 0);
        pool.Create("Folder/Sub/file.txt", 0x20);
        file_id = pool.Stat("Folder/Sub/file.txt").id;
        const std::array<std::byte, 4> data{
            std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
        require(pool.Write("Folder/Sub/file.txt", 0, data) == data.size(),
                "nested file write failed");
        bool unsupported_rejected = false;
        try { pool.Overwrite("Folder/Sub/file.txt", 0x400, true); }
        catch (const poolfs::Error& error) {
            unsupported_rejected = error.code() == poolfs::ErrorCode::InvalidArgument;
        }
        require(unsupported_rejected && pool.Stat("Folder/Sub/file.txt").size == 4,
                "unsupported overwrite attributes destroyed file contents");
        bool directory_write_rejected = false;
        try { (void)pool.Write("Folder/Sub", 0, data); }
        catch (const poolfs::Error& error) {
            directory_write_rejected = error.code() == poolfs::ErrorCode::InvalidArgument;
        }
        require(directory_write_rejected,
                "directory write was not rejected before journal commit");
        bool directory_truncate_rejected = false;
        try { pool.Truncate("Folder/Sub", 0); }
        catch (const poolfs::Error& error) {
            directory_truncate_rejected = error.code() == poolfs::ErrorCode::InvalidArgument;
        }
        require(directory_truncate_rejected,
                "directory truncate was not rejected before journal commit");
        pool.SetBasicInfo(file_id, 0x22, 132000000000000000ULL, 0,
                          132000000100000000ULL, 0);
        pool.Rename("Folder", "Archive");
        require(pool.NameForId(directory_id) == "Archive" &&
                    pool.NameForId(file_id) == "Archive/Sub/file.txt",
                "stable IDs changed after subtree rename");
        const auto children = pool.ListDirectory("Archive/Sub");
        require(children.size() == 1 && children[0].name == "file.txt" &&
                    children[0].id == file_id,
                "nested directory listing failed");
        pool.Overwrite("Archive/Sub/file.txt", 0x21, true);
        require(pool.Stat("Archive/Sub/file.txt").size == 0 &&
                    pool.Stat("Archive/Sub/file.txt").attributes == 0x21,
                "atomic overwrite did not truncate and replace attributes");
        bool denied = false;
        try { (void)pool.Write("Archive/Sub/file.txt", 0, data); }
        catch (const poolfs::Error& error) {
            denied = error.code() == poolfs::ErrorCode::AccessDenied;
        }
        require(denied, "read-only overwrite target accepted a write");
        pool.SetBasicInfo(file_id, 0x20, 0, 0, 0, 0);
        require(pool.Write("Archive/Sub/file.txt", 0, data) == data.size(),
                "write after clearing read-only failed");
        bool nonempty_rejected = false;
        try { pool.Remove("Archive"); }
        catch (const poolfs::Error& error) {
            nonempty_rejected = error.code() == poolfs::ErrorCode::InvalidArgument;
        }
        require(nonempty_rejected, "nonempty directory was deleted");
        pool.MakeDirectory(long_first);
        pool.MakeDirectory(long_first + '/' + long_second);
        pool.Create(long_path);
        require(pool.Write(long_path, 0, data) == data.size(),
                "path over 255 UTF-8 bytes failed");
        pool.Create(unicode_name);
        for (const std::string& bad : {std::string("CON.txt"),
                                      std::string("bad\\name"),
                                      std::string("COM\xC2\xB9.txt"),
                                      std::string("\xC0\xAF")}) {
            bool rejected = false;
            try { pool.Create(bad); }
            catch (const poolfs::Error& error) {
                rejected = error.code() == poolfs::ErrorCode::InvalidName;
            }
            require(rejected, "invalid Windows or UTF-8 name was accepted");
        }
    }
    run_checkpoint_child(members, L"--namespace-checkpoint-child");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        const auto file = pool.Stat("Archive/Sub/file.txt");
        require(file.id == file_id && file.attributes == 0x20 &&
                    file.creation_time == 132000000000000000ULL &&
                    file.last_write_time != 0 && file.size == 4,
                "file identity or metadata changed after reopen");
        require(pool.Stat("Archive").id == directory_id &&
                    pool.Stat("Archive").attributes == 0x12 &&
                    pool.Stat("Archive").creation_time == 132000000200000000ULL &&
                    pool.NameForId(directory_id) == "Archive",
                "directory identity or metadata changed after checkpoint reopen");
        require(pool.Stat(long_path).size == 4 &&
                    pool.Stat(unicode_name).id != 0,
                "long or Unicode path disappeared after reopen");
        pool.Remove("Archive/Sub/file.txt");
        pool.Remove("Archive/Sub");
        pool.Remove("Archive");
        pool.Remove("NormalDir");
        require(pool.IsDirectoryEmpty("") == false,
                "root directory unexpectedly became empty");
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "v6 namespace metadata test did not pass scrub");
}

void check_atomic_replace(const fs::path& scrubber,
                          const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    const auto source_bytes = pattern(0x14579231U);
    const auto target_bytes = pattern(0xF18022B7U);
    std::uint64_t source_id = 0;
    std::uint64_t target_id = 0;
    {
        Pool pool(Pool::V6ImageTag{}, members);
        pool.MakeDirectory("from");
        pool.MakeDirectory("to");
        pool.MakeDirectory("to/subdir");
        pool.Create("from/source.bin");
        pool.Create("to/target.bin");
        source_id = pool.Stat("from/source.bin").id;
        target_id = pool.Stat("to/target.bin").id;
        require(pool.Write("from/source.bin", 0, source_bytes) == source_bytes.size() &&
                    pool.Write("to/target.bin", 0, target_bytes) == target_bytes.size(),
                "cannot seed atomic replacement fixture");
        pool.Rename("from/source.bin", "from/source.bin", true);
        require(pool.NameForId(source_id) == "from/source.bin",
                "same-name rename changed source identity");
        bool rejected = false;
        try { pool.Rename("from/source.bin", "to/subdir", true); }
        catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::AccessDenied;
        }
        require(rejected, "file replaced a directory");
        rejected = false;
        try { pool.Rename("to/subdir", "to/target.bin", true); }
        catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::AccessDenied;
        }
        require(rejected, "directory replaced a file");
        rejected = false;
        try { pool.Rename("from/source.bin", "missing/target.bin", true); }
        catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::NotFound;
        }
        require(rejected, "replacement accepted a missing parent");
        pool.SetBasicInfo(target_id, 0x21, 0, 0, 0, 0);
        rejected = false;
        try { pool.Rename("from/source.bin", "to/target.bin", true); }
        catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::AccessDenied;
        }
        require(rejected && pool.NameForId(source_id) == "from/source.bin" &&
                    pool.NameForId(target_id) == "to/target.bin",
                "read-only replacement changed the namespace");
        pool.SetBasicInfo(target_id, 0x20, 0, 0, 0, 0);
        pool.Rename("from/source.bin", "to/target.bin", true);
        require(pool.Stat("to/target.bin").id == source_id &&
                    pool.NameForId(source_id) == "to/target.bin",
                "replacement did not retain the source file ID");
        rejected = false;
        try { (void)pool.NameForId(target_id); }
        catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::NotFound;
        }
        require(rejected, "replacement did not retire the target file ID");
        std::vector<std::byte> actual(source_bytes.size());
        require(pool.Read("to/target.bin", 0, actual) == actual.size() &&
                    actual == source_bytes,
                "replacement did not publish the source bytes");
        pool.Create("from/other.bin");
        pool.Rename("from/other.bin", "to/absent.bin", true);
        require(pool.Stat("to/absent.bin").id != 0,
                "replace flag failed to rename to an absent destination");
    }
    run_checkpoint_child(members, L"--replace-snapshot-child");
    require(selected_checkpoint(members[0]).generation > 1,
            "replacement was not serialized into a later checkpoint");
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> actual(source_bytes.size());
        require(pool.Read("to/target.bin", 0, actual) == actual.size() &&
                    actual == source_bytes && pool.Stat("to/target.bin").id == source_id,
                "replacement did not persist across reopen");
        bool missing = false;
        try { (void)pool.Stat("from/source.bin"); }
        catch (const poolfs::Error& error) {
            missing = error.code() == poolfs::ErrorCode::NotFound;
        }
        require(missing, "source path reappeared after replacement reopen");
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "atomic replacement did not pass scrub");
}

void check_replace_checkpoint_rollback(const fs::path& scrubber,
                                       const std::vector<fs::path>& members) {
    Pool::FormatV6Images(members, kMemberBytes);
    run_checkpoint_child(members, L"--replace-checkpoint-child");
    const auto protected_cp = selected_checkpoint(members[0]);
    require(protected_cp.generation >= 2,
            "replace fixture did not publish a checkpoint");
    const auto source_bytes = pattern(0x295FE036U);
    const auto target_bytes = pattern(0x417B5ED2U);
    std::uint64_t source_id = 0;
    std::uint64_t target_id = 0;
    {
        Pool pool(Pool::V6ImageTag{}, members);
        source_id = pool.Stat("source.bin").id;
        target_id = pool.Stat("target.bin").id;
        const auto available_before = pool.Available();
        pool.Rename("source.bin", "target.bin", true);
        require(pool.Available() == available_before,
                "checkpoint target extents were released for reuse");
        pool.Create("later.bin");
        const auto later = pattern(0xA25102D9U);
        require(pool.Write("later.bin", 0, later) == later.size(),
                "cannot write after checkpoint replacement");
    }
    require(selected_checkpoint(members[0]).generation == protected_cp.generation,
            "replace fixture rotated away the protected checkpoint");
    clear_journal_epoch(members, protected_cp.epoch);
    {
        Pool pool(Pool::V6ImageTag{}, members);
        std::vector<std::byte> actual(source_bytes.size());
        require(pool.Read("source.bin", 0, actual) == actual.size() &&
                    actual == source_bytes && pool.Stat("source.bin").id == source_id,
                "checkpoint source was lost after replacement rollback");
        require(pool.Read("target.bin", 0, actual) == actual.size() &&
                    actual == target_bytes && pool.Stat("target.bin").id == target_id,
                "checkpoint target was overwritten after replacement rollback");
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "replacement rollback did not pass scrub");
}
} // namespace

int main(int argc, char** argv) {
    if (argc == 4 && std::string_view(argv[1]) == "--tear-child") {
        Pool pool(Pool::V6ImageTag{}, {fs::path(argv[2]), fs::path(argv[3])});
        const std::array<std::byte, 3> replacement{
            std::byte{'B'}, std::byte{'B'}, std::byte{'B'}};
        pool.Write("target.bin", 4095, replacement);
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--checkpoint-child") {
        Pool pool(Pool::V6ImageTag{}, {fs::path(argv[2]), fs::path(argv[3])});
        pool.Create("snapshot.bin");
        const auto original = pattern(0x1BADB002U);
        require(pool.Write("snapshot.bin", 0, original) == original.size(),
                "checkpoint child could not seed file");
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--checkpoint-rotate-child") {
        Pool pool(Pool::V6ImageTag{}, {fs::path(argv[2]), fs::path(argv[3])});
        const std::array<std::byte, 1> patch{std::byte{'R'}};
        require(pool.Write("bulk.bin", 0, patch) == patch.size(),
                "checkpoint child could not rotate bulk file");
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--namespace-checkpoint-child") {
        Pool pool(Pool::V6ImageTag{}, {fs::path(argv[2]), fs::path(argv[3])});
        const std::array<std::byte, 4> data{
            std::byte{'d'}, std::byte{'a'}, std::byte{'t'}, std::byte{'a'}};
        require(pool.Write("Archive/Sub/file.txt", 0, data) == data.size(),
                "namespace checkpoint child could not write existing file");
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--replace-checkpoint-child") {
        Pool pool(Pool::V6ImageTag{}, {fs::path(argv[2]), fs::path(argv[3])});
        const auto source_bytes = pattern(0x295FE036U);
        const auto target_bytes = pattern(0x417B5ED2U);
        pool.Create("target.bin");
        require(pool.Write("target.bin", 0, target_bytes) == target_bytes.size(),
                "checkpoint child could not write replace target");
        pool.Create("source.bin");
        require(pool.Write("source.bin", 0, source_bytes) == source_bytes.size(),
                "checkpoint child could not write replace source");
        return 0;
    }
    if (argc == 4 && std::string_view(argv[1]) == "--replace-snapshot-child") {
        Pool pool(Pool::V6ImageTag{}, {fs::path(argv[2]), fs::path(argv[3])});
        const auto source_bytes = pattern(0x14579231U);
        require(pool.Write("to/target.bin", 0, source_bytes) == source_bytes.size(),
                "checkpoint child could not rewrite replaced file");
        return 0;
    }
    require(argc == 1, "usage: poolfs-v6-integrity-tests.exe");
    require(std::getenv("POOLFS_TEST_V6_TEAR_CHECKSUM_PAGE") == nullptr,
            "unset torn-page test hook before running v6 tests");
    require(std::getenv("POOLFS_TEST_V6_CHECKPOINT_AFTER_WRITE") == nullptr,
            "unset checkpoint test hook before running v6 tests");
    const auto scrubber = sibling_scrubber();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() /
        ("poolfs-v6-integrity-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const std::vector<fs::path> normal{dir / "normal-a.img", dir / "normal-b.img"};
    const std::vector<fs::path> corrupt{dir / "corrupt-a.img", dir / "corrupt-b.img"};
    const std::vector<fs::path> torn{dir / "torn-a.img", dir / "torn-b.img"};
    const std::vector<fs::path> stale{dir / "stale-a.img", dir / "stale-b.img"};
    const std::vector<fs::path> mixed{dir / "mixed-small.img", dir / "mixed-large.img"};
    const std::vector<fs::path> checkpoint_data{
        dir / "checkpoint-data-a.img", dir / "checkpoint-data-b.img"};
    const std::vector<fs::path> checkpoint_pages{
        dir / "checkpoint-pages-a.img", dir / "checkpoint-pages-b.img"};
    const std::vector<fs::path> checkpoint_space{
        dir / "checkpoint-space-a.img", dir / "checkpoint-space-b.img"};
    const std::vector<fs::path> old_layout{
        dir / "old-layout-a.img", dir / "old-layout-b.img"};
    const std::vector<fs::path> namespace_pool{
        dir / "namespace-a.img", dir / "namespace-b.img"};
    const std::vector<fs::path> replace_pool{
        dir / "replace-a.img", dir / "replace-b.img"};
    const std::vector<fs::path> replace_checkpoint{
        dir / "replace-checkpoint-a.img", dir / "replace-checkpoint-b.img"};
    const auto cleanup = [&] {
        for (const auto& path : normal) fs::remove(path);
        for (const auto& path : corrupt) fs::remove(path);
        for (const auto& path : torn) fs::remove(path);
        for (const auto& path : stale) fs::remove(path);
        for (const auto& path : mixed) fs::remove(path);
        for (const auto& path : checkpoint_data) fs::remove(path);
        for (const auto& path : checkpoint_pages) fs::remove(path);
        for (const auto& path : checkpoint_space) fs::remove(path);
        for (const auto& path : old_layout) fs::remove(path);
        for (const auto& path : namespace_pool) fs::remove(path);
        for (const auto& path : replace_pool) fs::remove(path);
        for (const auto& path : replace_checkpoint) fs::remove(path);
        fs::remove(dir); // Never recursively remove an unexpected file.
    };
    try {
        check_normal_io(scrubber, normal);
        for (const auto& path : normal) fs::remove(path);
        check_corrupt_data(scrubber, corrupt);
        for (const auto& path : corrupt) fs::remove(path);
        check_torn_checksum_page(scrubber, torn);
        for (const auto& path : torn) fs::remove(path);
        check_stale_root_reuse(scrubber, stale);
        for (const auto& path : stale) fs::remove(path);
        check_mixed_size_io(scrubber, mixed);
        for (const auto& path : mixed) fs::remove(path);
        check_checkpoint_retention(scrubber, checkpoint_data, checkpoint_pages);
        for (const auto& path : checkpoint_data) fs::remove(path);
        for (const auto& path : checkpoint_pages) fs::remove(path);
        check_checkpoint_space_recovery(scrubber, checkpoint_space);
        for (const auto& path : checkpoint_space) fs::remove(path);
        check_old_layout_rejection(scrubber, old_layout);
        for (const auto& path : old_layout) fs::remove(path);
        check_namespace_metadata(scrubber, namespace_pool);
        for (const auto& path : namespace_pool) fs::remove(path);
        check_atomic_replace(scrubber, replace_pool);
        for (const auto& path : replace_pool) fs::remove(path);
        check_replace_checkpoint_rollback(scrubber, replace_checkpoint);
        for (const auto& path : replace_checkpoint) fs::remove(path);
        std::cout << "poolfs v6 data integrity tests passed\n";
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
