#include "core.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
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

namespace {
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kMemberBytes = 512 * kMiB;
constexpr std::uint64_t kBlockBytes = 4096;
constexpr std::uint64_t kDataStart = 256 * kMiB;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

class UniqueHandle {
public:
    explicit UniqueHandle(HANDLE value = INVALID_HANDLE_VALUE) : value_(value) {}
    ~UniqueHandle() { reset(); }
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
    DWORD exit_code;
    std::string output;
};

fs::path sibling_scrubber() {
    std::vector<wchar_t> module_path(32768);
    const auto length = GetModuleFileNameW(nullptr, module_path.data(),
                                           static_cast<DWORD>(module_path.size()));
    require(length && length < module_path.size(), "cannot locate v5 test executable");
    const fs::path executable(module_path.data(), module_path.data() + length);
    const auto scrubber = executable.parent_path() / L"poolfs-scrub.exe";
    require(fs::is_regular_file(scrubber), "poolfs-scrub.exe is missing beside v5 tests");
    return scrubber;
}

ProcessResult run_scrubber(const fs::path& scrubber,
                           const std::vector<fs::path>& members) {
    SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
    HANDLE read_raw = INVALID_HANDLE_VALUE;
    HANDLE write_raw = INVALID_HANDLE_VALUE;
    require(CreatePipe(&read_raw, &write_raw, &security, 0) != 0,
            "cannot create scrub output pipe");
    UniqueHandle read_pipe(read_raw);
    UniqueHandle write_pipe(write_raw);
    require(SetHandleInformation(read_pipe.get(), HANDLE_FLAG_INHERIT, 0) != 0,
            "cannot make scrub read pipe private");

    std::wstring command = L"\"" + scrubber.wstring() + L"\"";
    for (const auto& member : members) {
        const auto path = member.wstring();
        require(path.find(L'\"') == std::wstring::npos,
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

std::vector<std::byte> pattern(std::uint8_t seed) {
    std::vector<std::byte> result(8192);
    std::uint32_t state = 0x9e3779b9U ^ seed;
    for (auto& value : result) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        value = static_cast<std::byte>(state & 0xff);
    }
    return result;
}

struct Location {
    std::size_t member;
    std::uint64_t offset;
};

// Look for a long data signature instead of assuming a private v5 data offset.
// The signature is not stored verbatim in the journal or checkpoint.
Location locate_data(const std::vector<fs::path>& members,
                     std::span<const std::byte> signature) {
    constexpr std::size_t kScanBytes = 4 * 1024 * 1024;
    require(signature.size() > 32 && signature.size() < kScanBytes,
            "invalid test signature");
    std::vector<char> chunk(kScanBytes + signature.size() - 1);
    std::vector<char> needle(signature.size());
    for (std::size_t i = 0; i < signature.size(); ++i)
        needle[i] = static_cast<char>(signature[i]);
    for (std::size_t member = 0; member < members.size(); ++member) {
        std::ifstream input(members[member], std::ios::binary);
        require(input.good(), "cannot open member image for corruption test");
        std::uint64_t consumed = 0;
        std::size_t carry = 0;
        while (consumed < kMemberBytes) {
            const auto take = static_cast<std::streamsize>(
                std::min<std::uint64_t>(kScanBytes, kMemberBytes - consumed));
            input.read(chunk.data() + carry, take);
            require(input.gcount() == take, "short member image during scan");
            const auto end = chunk.begin() + carry + static_cast<std::size_t>(take);
            const auto found = std::search(chunk.begin(), end,
                                           needle.begin(), needle.end());
            if (found != end) {
                const auto position = static_cast<std::uint64_t>(
                    std::distance(chunk.begin(), found));
                return {member, consumed - carry + position};
            }
            carry = std::min(signature.size() - 1,
                             carry + static_cast<std::size_t>(take));
            std::copy(end - carry, end, chunk.begin());
            consumed += static_cast<std::uint64_t>(take);
        }
    }
    throw std::runtime_error("could not locate committed file data in members");
}

void flip_byte(const fs::path& path, std::uint64_t offset) {
    std::fstream image(path, std::ios::binary | std::ios::in | std::ios::out);
    require(image.good(), "cannot open member image for byte flip");
    image.seekg(static_cast<std::streamoff>(offset));
    char old = 0;
    image.get(old);
    require(image.good(), "cannot read byte for flip");
    image.seekp(static_cast<std::streamoff>(offset));
    image.put(static_cast<char>(old ^ 0x01));
    image.flush();
    require(image.good(), "cannot write flipped byte");
}

std::uint64_t checksum_cell_for(std::uint64_t data_offset) {
    require(data_offset >= kDataStart && data_offset < kMemberBytes,
            "located payload is outside v5 data area");
    const auto blocks = (kMemberBytes - kDataStart) / (kBlockBytes + 8);
    const auto checksum_bytes = blocks * 8;
    const auto checksum_start = kMemberBytes -
        ((checksum_bytes + kBlockBytes - 1) / kBlockBytes) * kBlockBytes;
    const auto block_index = (data_offset - kDataStart) / kBlockBytes;
    require(block_index < blocks, "located payload is outside v5 data blocks");
    return checksum_start + block_index * 8;
}

void check_normal_io(const fs::path& scrubber,
                     const std::vector<fs::path>& members) {
    Pool::FormatV5Images(members, kMemberBytes);
    auto expected = pattern(11);
    {
        Pool pool(Pool::V5ImageTag{}, members);
        require(pool.Capacity() >= 500 * kMiB && pool.Capacity() < 512 * kMiB,
                "unexpected v5 capacity");
        pool.Create("unaligned.bin");
        require(pool.Write("unaligned.bin", 0, expected) == expected.size(),
                "initial write was short");
        const std::array<std::byte, 3> patch{std::byte{'X'}, std::byte{'Y'}, std::byte{'Z'}};
        require(pool.Write("unaligned.bin", 4095, patch) == patch.size(),
                "unaligned overwrite was short");
        std::copy(patch.begin(), patch.end(), expected.begin() + 4095);
        const auto busy = run_scrubber(scrubber, members);
        require(busy.exit_code == 1 &&
                    busy.output.find("status: ERROR") != std::string::npos,
                "scrub did not refuse in-use member images");
    }
    {
        Pool pool(Pool::V5ImageTag{}, {members[1], members[0]});
        std::vector<std::byte> actual(expected.size());
        require(pool.Read("unaligned.bin", 0, actual) == actual.size() && actual == expected,
                "data changed after reopening with reversed member paths");
        pool.Truncate("unaligned.bin", 4100);
        pool.Truncate("unaligned.bin", 8192);
    }
    {
        Pool pool(Pool::V5ImageTag{}, members);
        std::vector<std::byte> actual(8192);
        require(pool.Read("unaligned.bin", 0, actual) == actual.size(),
                "reopened read was short");
        require(std::equal(actual.begin(), actual.begin() + 4100, expected.begin()),
                "truncated prefix changed");
        require(std::all_of(actual.begin() + 4100, actual.end(),
                    [](std::byte value) { return value == std::byte{0}; }),
                "regrown gap was not zero-filled");
    }
    {
        Pool pool(Pool::V5ReadOnlyImageTag{}, members);
        require(pool.Scrub({}) == 0, "clean v5 pool failed scrub");
    }
    const auto clean = run_scrubber(scrubber, members);
    require(clean.exit_code == 0 &&
                clean.output.find("status: CLEAN") != std::string::npos,
            "scrub CLI did not report a clean pool");
}

void check_corrupt_data(const fs::path& scrubber,
                        const std::vector<fs::path>& members) {
    Pool::FormatV5Images(members, kMemberBytes);
    const auto first = pattern(71);
    const auto second = pattern(83);
    {
        Pool pool(Pool::V5ImageTag{}, members);
        pool.Create("first.bin");
        pool.Create("second.bin");
        require(pool.Write("first.bin", 0, first) == first.size(), "first write was short");
        require(pool.Write("second.bin", 0, second) == second.size(), "second write was short");
    }
    const auto first_location = locate_data(members,
        std::span<const std::byte>(first).subspan(128, 64));
    const auto second_location = locate_data(members,
        std::span<const std::byte>(second).subspan(128, 64));
    require(first_location.member != second_location.member,
            "test files did not span both pool members");
    flip_byte(members[second_location.member], second_location.offset + 16);
    const auto damaged = run_scrubber(scrubber, members);
    require(damaged.exit_code == 2 &&
                damaged.output.find("status: DAMAGED") != std::string::npos &&
                damaged.output.find("file=second.bin") != std::string::npos &&
                damaged.output.find("member=" + std::to_string(second_location.member)) !=
                    std::string::npos,
            "scrub CLI did not report damaged file and member");
    {
        Pool pool(Pool::V5ReadOnlyImageTag{}, members);
        std::vector<poolfs::ScrubIssue> issues;
        const auto count = pool.Scrub([&](const poolfs::ScrubIssue& issue) {
            issues.push_back(issue);
        });
        require(count == 1 && issues.size() == 1 && issues[0].name == "second.bin" &&
                    issues[0].member == second_location.member,
                "scrub did not identify the damaged file and member");
    }
    {
        Pool pool(Pool::V5ImageTag{}, members);
        std::vector<std::byte> actual(first.size());
        require(pool.Read("first.bin", 0, actual) == actual.size() && actual == first,
                "healthy file did not remain readable");
        bool rejected = false;
        try {
            std::array<std::byte, 16> affected{};
            pool.Read("second.bin", 144, affected);
        } catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::Corrupt;
        }
        require(rejected, "damaged file data did not raise Corrupt on read");
    }
    // Restore the data and prove that damaging its separate CRC cell is also
    // detected. A checksum table needs the same fail-closed read behavior.
    flip_byte(members[second_location.member], second_location.offset + 16);
    {
        Pool pool(Pool::V5ImageTag{}, members);
        std::vector<std::byte> actual(second.size());
        require(pool.Read("second.bin", 0, actual) == actual.size() && actual == second,
                "restoring data did not restore a valid read");
    }
    flip_byte(members[second_location.member],
              checksum_cell_for(second_location.offset));
    const auto damaged_cell = run_scrubber(scrubber, members);
    require(damaged_cell.exit_code == 2 &&
                damaged_cell.output.find("status: DAMAGED") != std::string::npos &&
                damaged_cell.output.find("file=second.bin") != std::string::npos,
            "scrub CLI missed a damaged checksum cell");
    {
        Pool pool(Pool::V5ImageTag{}, members);
        bool rejected = false;
        try {
            std::array<std::byte, 16> affected{};
            pool.Read("second.bin", 144, affected);
        } catch (const poolfs::Error& error) {
            rejected = error.code() == poolfs::ErrorCode::Corrupt;
        }
        require(rejected, "damaged checksum cell did not raise Corrupt on read");
    }
    {
        Pool pool(Pool::V5ReadOnlyImageTag{}, members);
        require(pool.Scrub({}) == 1, "scrub missed a damaged checksum cell");
    }
}
} // namespace

int main() {
    const auto scrubber = sibling_scrubber();
    const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto dir = fs::temp_directory_path() /
        ("poolfs-v5-integrity-test-" + std::to_string(stamp));
    fs::create_directories(dir);
    const std::vector<fs::path> normal{dir / "normal-a.img", dir / "normal-b.img"};
    const std::vector<fs::path> corrupt{dir / "corrupt-a.img", dir / "corrupt-b.img"};
    const auto cleanup = [&] {
        for (const auto& path : normal) fs::remove(path);
        for (const auto& path : corrupt) fs::remove(path);
        fs::remove(dir); // Never recursively remove an unexpected file.
    };
    try {
        check_normal_io(scrubber, normal);
        for (const auto& path : normal) fs::remove(path);
        check_corrupt_data(scrubber, corrupt);
        std::cout << "poolfs v5 data integrity tests passed\n";
    } catch (...) {
        cleanup();
        throw;
    }
    cleanup();
}
