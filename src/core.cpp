#include "core.h"
#include "crypto.h"
#include "member_device.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <stdexcept>
#include <string_view>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <fcntl.h>
#include <io.h>
#include <share.h>
#else
#include <fcntl.h>
#include <unistd.h>
#endif

namespace poolfs {
namespace {

using Bytes = std::vector<std::byte>;
constexpr std::uint64_t kHeaderBytes = 4096;
constexpr std::size_t kRecordHeaderBytes = 20;
struct Geometry {
    std::uint32_t version;
    std::uint64_t journal_offset;
    std::uint64_t journal_bytes;
    std::uint64_t data_offset;
    std::uint32_t io_block;
    std::array<char, 8> header_magic;
    std::array<char, 4> record_magic;
};
constexpr Geometry kImageV1{1, 2 * kHeaderBytes, 2ULL * 1024 * 1024,
    4ULL * 1024 * 1024, 1, {'P','O','O','L','F','S','0','1'},
    {'P','J','R','1'}};
constexpr Geometry kRawV2{2, 2 * kHeaderBytes, 64ULL * 1024 * 1024,
    128ULL * 1024 * 1024, 4096, {'P','O','O','L','F','S','0','2'},
    {'P','J','R','2'}};
constexpr Geometry kRawV3{3, 2 * kHeaderBytes, 64ULL * 1024 * 1024,
    128ULL * 1024 * 1024, 4096, {'P','O','O','L','F','S','0','3'},
    {'P','J','R','3'}};
constexpr Geometry kRawV4{4, 2 * kHeaderBytes, 64ULL * 1024 * 1024,
    256ULL * 1024 * 1024, 4096, {'P','O','O','L','F','S','0','4'},
    {'P','J','R','4'}};
constexpr Geometry kRawV5{5, 2 * kHeaderBytes, 64ULL * 1024 * 1024,
    256ULL * 1024 * 1024, 4096, {'P','O','O','L','F','S','0','5'},
    {'P','J','R','5'}};
constexpr Geometry kRawV6{6, 2 * kHeaderBytes, 64ULL * 1024 * 1024,
    256ULL * 1024 * 1024, 4096, {'P','O','O','L','F','S','0','6'},
    {'P','J','R','6'}};
constexpr std::uint64_t kV6GroupBlocks = 512;
// Byte 46 of each v6 header distinguishes layouts that protect checkpoint
// data and checksum roots from reuse during later journal operations.
constexpr std::uint8_t kV6LayoutRevision = 2;
constexpr std::uint8_t kV6EncryptedLayoutRevision = 3;
constexpr std::size_t kEncryptionSaltOffset = 512;
constexpr std::size_t kEncryptionSaltBytes = 32;
constexpr std::size_t kEncryptionIterationsOffset = 544;
constexpr std::size_t kEncryptionMacOffset = 548;
constexpr std::size_t kEncryptionMacBytes = 32;
constexpr std::uint32_t kEncryptionIterations = 600000;
constexpr std::uint32_t kFileAttributeDirectory = 0x10;
constexpr std::uint32_t kFileAttributeNormal = 0x80;
constexpr std::uint32_t kFileAttributeReadOnly = 0x01;
constexpr std::uint32_t kAllowedAttributes = 0x01 | 0x02 | 0x04 | 0x10 | 0x20 | 0x80;
constexpr std::uint32_t kUnchangedAttributes = 0xffffffffU;
bool valid_attributes(std::uint32_t attributes, bool directory) {
    return attributes && !(attributes & ~kAllowedAttributes) &&
        bool(attributes & kFileAttributeDirectory) == directory &&
        (!(attributes & kFileAttributeNormal) || attributes == kFileAttributeNormal);
}
std::uint32_t normalized_attributes(std::uint32_t attributes, bool directory) {
    if (!attributes) attributes = directory ? kFileAttributeDirectory : kFileAttributeNormal;
    if (directory || (attributes & ~kFileAttributeNormal))
        attributes &= ~kFileAttributeNormal;
    if (directory) attributes |= kFileAttributeDirectory;
    if (!valid_attributes(attributes, directory))
        throw Error(ErrorCode::InvalidArgument, "unsupported file attributes");
    return attributes;
}
constexpr std::size_t kMaximumV3Members = 16;
constexpr std::size_t kManifestOffset = 64;
constexpr std::size_t kManifestEntryBytes = 24;
constexpr std::uint64_t kV4EpochBytes = kRawV4.journal_bytes / 2;
constexpr std::uint64_t kV4CheckpointStart = kRawV4.journal_offset + kRawV4.journal_bytes;
constexpr std::uint64_t kV4CheckpointSlotBytes =
    ((kRawV4.data_offset - kV4CheckpointStart) / 2 / kRawV4.io_block) * kRawV4.io_block;
constexpr std::uint64_t v4_epoch_offset(std::uint8_t epoch) {
    return kRawV4.journal_offset + static_cast<std::uint64_t>(epoch) * kV4EpochBytes;
}
constexpr std::uint64_t v4_checkpoint_offset(std::uint8_t slot) {
    return kV4CheckpointStart + static_cast<std::uint64_t>(slot) * kV4CheckpointSlotBytes;
}
std::uint64_t allocation_bytes(std::uint64_t bytes, const Geometry& g) {
    const auto block = static_cast<std::uint64_t>(g.io_block);
    if (bytes > std::numeric_limits<std::uint64_t>::max() - (block - 1))
        throw Error(ErrorCode::InvalidArgument, "allocation size overflow");
    return ((bytes + block - 1) / block) * block;
}
std::uint64_t v5_data_blocks(std::uint64_t member_bytes) {
    const auto available = member_bytes - kRawV5.data_offset;
    auto blocks = available / (kRawV5.io_block + 8ULL);
    while (blocks * kRawV5.io_block + allocation_bytes(blocks * 8, kRawV5) > available)
        --blocks;
    return blocks;
}
std::uint64_t v5_data_end(std::uint64_t member_bytes) {
    return kRawV5.data_offset + v5_data_blocks(member_bytes) * kRawV5.io_block;
}
std::uint64_t v5_checksum_start(std::uint64_t member_bytes) {
    return member_bytes - allocation_bytes(v5_data_blocks(member_bytes) * 8, kRawV5);
}
std::uint64_t v6_groups(std::uint64_t blocks) {
    return (blocks + kV6GroupBlocks - 1) / kV6GroupBlocks;
}
std::uint64_t v6_data_blocks(std::uint64_t member_bytes) {
    const auto available = member_bytes - kRawV6.data_offset;
    std::uint64_t low = 0;
    std::uint64_t high = available / kRawV6.io_block;
    while (low < high) {
        const auto mid = low + (high - low + 1) / 2;
        const auto occupied = mid * kRawV6.io_block +
            2 * v6_groups(mid) * kRawV6.io_block;
        if (occupied <= available) low = mid;
        else high = mid - 1;
    }
    return low;
}
std::uint64_t v6_data_end(std::uint64_t member_bytes) {
    return kRawV6.data_offset + v6_data_blocks(member_bytes) * kRawV6.io_block;
}
std::uint64_t v6_checksum_start(std::uint64_t member_bytes) {
    return member_bytes - 2 * v6_groups(v6_data_blocks(member_bytes)) * kRawV6.io_block;
}
std::uint64_t v6_checksum_page(std::uint64_t member_bytes,
                               std::uint64_t group, std::uint8_t slot) {
    return v6_checksum_start(member_bytes) + (2 * group + slot) * kRawV6.io_block;
}
std::uint64_t crc64(const std::byte* data, std::size_t count) {
    // CRC-64/XZ; this detects accidental damage, not deliberate modification.
    static_assert(std::endian::native == std::endian::little);
    static const auto tables = [] {
        std::array<std::array<std::uint64_t, 256>, 8> entries{};
        for (std::size_t i = 0; i < entries[0].size(); ++i) {
            std::uint64_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value >> 1) ^ ((value & 1) ? 0xC96C5795D7870F42ULL : 0);
            entries[0][i] = value;
        }
        for (std::size_t slice = 1; slice < entries.size(); ++slice)
            for (std::size_t i = 0; i < entries[slice].size(); ++i) {
                const auto previous = entries[slice - 1][i];
                entries[slice][i] = entries[0][previous & 0xff] ^ (previous >> 8);
            }
        return entries;
    }();
    std::uint64_t value = ~std::uint64_t{0};
    while (count >= 8) {
        std::uint64_t word = 0;
        std::memcpy(&word, data, sizeof(word));
        value ^= word;
        value = tables[7][value & 0xff] ^ tables[6][(value >> 8) & 0xff] ^
                tables[5][(value >> 16) & 0xff] ^ tables[4][(value >> 24) & 0xff] ^
                tables[3][(value >> 32) & 0xff] ^ tables[2][(value >> 40) & 0xff] ^
                tables[1][(value >> 48) & 0xff] ^ tables[0][value >> 56];
        data += 8;
        count -= 8;
    }
    for (std::size_t i = 0; i < count; ++i)
        value = tables[0][(value ^ std::to_integer<std::uint8_t>(data[i])) & 0xff] ^
                (value >> 8);
    return ~value;
}

void put8(Bytes& b, std::uint8_t v) { b.push_back(static_cast<std::byte>(v)); }
void put16(Bytes& b, std::uint16_t v) {
    for (int i = 0; i < 2; ++i) put8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}
void put32(Bytes& b, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) put8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}
void put64(Bytes& b, std::uint64_t v) {
    for (int i = 0; i < 8; ++i) put8(b, static_cast<std::uint8_t>(v >> (8 * i)));
}
std::uint8_t byte_at(const Bytes& b, std::size_t p) {
    return std::to_integer<std::uint8_t>(b.at(p));
}
std::uint16_t u16_at(const Bytes& b, std::size_t p) {
    return std::uint16_t(byte_at(b, p)) | (std::uint16_t(byte_at(b, p + 1)) << 8);
}
std::uint32_t u32_at(const Bytes& b, std::size_t p) {
    std::uint32_t v = 0;
    for (int i = 0; i < 4; ++i) v |= std::uint32_t(byte_at(b, p + i)) << (8 * i);
    return v;
}
std::uint64_t u64_at(const Bytes& b, std::size_t p) {
    std::uint64_t v = 0;
    for (int i = 0; i < 8; ++i) v |= std::uint64_t(byte_at(b, p + i)) << (8 * i);
    return v;
}
void overwrite32(Bytes& b, std::size_t p, std::uint32_t v) {
    for (int i = 0; i < 4; ++i) b.at(p + i) = static_cast<std::byte>(v >> (8 * i));
}
std::uint32_t fnv1a(const std::byte* p, std::size_t n) {
    std::uint32_t v = 2166136261U;
    for (std::size_t i = 0; i < n; ++i) {
        v ^= std::to_integer<std::uint8_t>(p[i]);
        v *= 16777619U;
    }
    return v;
}
std::uint64_t fnv1a64(const std::byte* p, std::size_t n) {
    std::uint64_t v = 14695981039346656037ULL;
    for (std::size_t i = 0; i < n; ++i) {
        v ^= std::to_integer<std::uint8_t>(p[i]);
        v *= 1099511628211ULL;
    }
    return v;
}
void put_name(Bytes& b, const std::string& name) {
    if (name.size() > 1024) throw Error(ErrorCode::InvalidName, "file path too long");
    put16(b, static_cast<std::uint16_t>(name.size()));
    for (unsigned char c : name) put8(b, c);
}
void validate_name(const std::string& name) {
    if (name.empty() || name == "." || name == ".." || name.size() > 255)
        throw Error(ErrorCode::InvalidName, "invalid file name");
    for (unsigned char c : name) {
        if (c < 32 || c == 127 || c == '/' || c == '\\')
            throw Error(ErrorCode::InvalidName, "invalid file name");
    }
}
std::size_t utf16_units(std::string_view text) {
    std::size_t units = 0;
    for (std::size_t i = 0; i < text.size();) {
        const auto first = static_cast<std::uint8_t>(text[i]);
        std::uint32_t value = 0;
        std::size_t count = 0;
        if (first < 0x80) { value = first; count = 1; }
        else if (first >= 0xc2 && first <= 0xdf) { value = first & 0x1f; count = 2; }
        else if (first >= 0xe0 && first <= 0xef) { value = first & 0x0f; count = 3; }
        else if (first >= 0xf0 && first <= 0xf4) { value = first & 0x07; count = 4; }
        else throw Error(ErrorCode::InvalidName, "invalid UTF-8 path");
        if (count > text.size() - i)
            throw Error(ErrorCode::InvalidName, "truncated UTF-8 path");
        for (std::size_t j = 1; j < count; ++j) {
            const auto next = static_cast<std::uint8_t>(text[i + j]);
            if ((next & 0xc0) != 0x80)
                throw Error(ErrorCode::InvalidName, "invalid UTF-8 path");
            value = (value << 6) | (next & 0x3f);
        }
        if ((count == 2 && value < 0x80) ||
            (count == 3 && value < 0x800) ||
            (count == 4 && value < 0x10000) ||
            (value >= 0xd800 && value <= 0xdfff) || value > 0x10ffff ||
            value < 32 || value == 127)
            throw Error(ErrorCode::InvalidName, "invalid Unicode path component");
        units += value > 0xffff ? 2 : 1;
        i += count;
    }
    return units;
}
std::string parent_path(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash);
}
std::string leaf_name(const std::string& path) {
    const auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}
void validate_v6_path(const std::string& path) {
    if (path.empty() || path.size() > 1024)
        throw Error(ErrorCode::InvalidName, "invalid v6 path length");
    std::size_t start = 0;
    while (start < path.size()) {
        const auto end = path.find('/', start);
        const auto stop = end == std::string::npos ? path.size() : end;
        const auto component = path.substr(start, stop - start);
        if (component.empty() || component == "." || component == ".." ||
            utf16_units(component) > 255)
            throw Error(ErrorCode::InvalidName, "invalid v6 path component");
        if (component.back() == '.' || component.back() == ' ')
            throw Error(ErrorCode::InvalidName, "invalid Windows path component");
        for (unsigned char c : component)
            if (c == '<' || c == '>' || c == ':' || c == '"' || c == '\\' || c == '|' ||
                c == '?' || c == '*')
                throw Error(ErrorCode::InvalidName, "invalid Windows path component");
        std::string base = component.substr(0, component.find('.'));
        for (auto& c : base)
            if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
        const bool superscript = base.size() == 5 &&
            static_cast<std::uint8_t>(base[3]) == 0xc2 &&
            (static_cast<std::uint8_t>(base[4]) == 0xb9 ||
             static_cast<std::uint8_t>(base[4]) == 0xb2 ||
             static_cast<std::uint8_t>(base[4]) == 0xb3);
        if (base == "CON" || base == "PRN" || base == "AUX" || base == "NUL" ||
            (base.size() == 4 &&
             (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT") &&
             base[3] >= '1' && base[3] <= '9') ||
            (superscript &&
             (base.substr(0, 3) == "COM" || base.substr(0, 3) == "LPT")))
            throw Error(ErrorCode::InvalidName, "reserved Windows device name");
        if (end == std::string::npos) break;
        start = end + 1;
        if (start == path.size())
            throw Error(ErrorCode::InvalidName, "empty path component");
    }
}
std::uint64_t current_filetime() {
#ifdef _WIN32
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    return (static_cast<std::uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime;
#else
    constexpr std::uint64_t kUnixToFiletimeSeconds = 11644473600ULL;
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return kUnixToFiletimeSeconds * 10000000ULL +
        static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(now).count() / 100);
#endif
}
struct Reader {
    const Bytes& b;
    std::size_t p = 0;
    void require(std::size_t n) const {
        if (p > b.size() || n > b.size() - p)
            throw Error(ErrorCode::Corrupt, "truncated journal payload");
    }
    std::uint8_t get8() {
        require(1);
        return byte_at(b, p++);
    }
    std::uint16_t get16() { require(2); auto v = u16_at(b, p); p += 2; return v; }
    std::uint32_t get32() { require(4); auto v = u32_at(b, p); p += 4; return v; }
    std::uint64_t get64() { require(8); auto v = u64_at(b, p); p += 8; return v; }
    std::string name(bool v6_path = false) {
        const auto n = get16();
        require(n);
        std::string s;
        s.reserve(n);
        for (std::size_t i = 0; i < n; ++i) s.push_back(static_cast<char>(get8()));
        if (v6_path) validate_v6_path(s);
        else validate_name(s);
        return s;
    }
    void done() const {
        if (p != b.size()) throw Error(ErrorCode::Corrupt, "invalid journal payload length");
    }
};

class DiskImage final : public MemberDevice {
public:
    explicit DiskImage(const std::filesystem::path& path, bool create = false,
                       bool read_only = false)
        : path_(path), read_only_(read_only) {
#ifdef _WIN32
        if (create) {
            HANDLE handle = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                                        0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
                throw Error(ErrorCode::Io, "cannot create new member image: " + path.string());
            const int fd = _open_osfhandle(reinterpret_cast<std::intptr_t>(handle),
                                          _O_RDWR | _O_BINARY);
            if (fd < 0) {
                CloseHandle(handle);
                throw Error(ErrorCode::Io, "cannot adopt new member image handle");
            }
            file_ = _fdopen(fd, "wb+");
            if (!file_) {
                _close(fd);
                throw Error(ErrorCode::Io, "cannot open new member image stream");
            }
        } else if (read_only) {
            file_ = _wfsopen(path.c_str(), L"rb", _SH_DENYRW);
            if (!file_)
                throw Error(ErrorCode::Io, "cannot open member image exclusively for scrub: " + path.string());
        } else if (!(file_ = _wfsopen(path.c_str(), L"r+b", _SH_DENYRW))) {
            throw Error(ErrorCode::Io, "cannot open member image: " + path.string());
        }
#else
        if (create) {
            const int fd = open(path.c_str(), O_CREAT | O_EXCL | O_RDWR, 0666);
            if (fd < 0)
                throw Error(ErrorCode::Io, "cannot create new member image: " + path.string());
            file_ = fdopen(fd, "wb+");
            if (!file_) {
                close(fd);
                throw Error(ErrorCode::Io, "cannot open new member image stream");
            }
        } else {
            file_ = std::fopen(path.string().c_str(), read_only ? "rb" : "r+b");
            if (!file_)
                throw Error(ErrorCode::Io, "cannot open member image: " + path.string());
        }
#endif
    }
    ~DiskImage() override { if (file_) std::fclose(file_); }
    DiskImage(const DiskImage&) = delete;
    DiskImage& operator=(const DiskImage&) = delete;

    void resize(std::uint64_t bytes) {
        if (read_only_) throw Error(ErrorCode::ReadOnly, "member image is read-only");
#ifdef _WIN32
        if (_chsize_s(_fileno(file_), bytes) != 0)
#else
        if (ftruncate(fileno(file_), static_cast<off_t>(bytes)) != 0)
#endif
            throw Error(ErrorCode::Io, "cannot size member image");
    }
    std::uint64_t size() const override { return std::filesystem::file_size(path_); }
    void read(std::uint64_t offset, std::byte* data, std::size_t length) override {
        seek(offset);
        if (std::fread(data, 1, length, file_) != length)
            throw Error(ErrorCode::Io, "member image read failed");
    }
    void write(std::uint64_t offset, const std::byte* data, std::size_t length) override {
        if (read_only_) throw Error(ErrorCode::ReadOnly, "member image is read-only");
        seek(offset);
        if (std::fwrite(data, 1, length, file_) != length)
            throw Error(ErrorCode::Io, "member image write failed");
    }
    void sync() override {
        if (read_only_) return;
        if (std::fflush(file_) != 0) throw Error(ErrorCode::Io, "member image flush failed");
#ifdef _WIN32
        if (_commit(_fileno(file_)) != 0)
#else
        if (fsync(fileno(file_)) != 0)
#endif
            throw Error(ErrorCode::Io, "member image durable flush failed");
    }
private:
    std::filesystem::path path_;
    std::FILE* file_ = nullptr;
    bool read_only_ = false;
    void seek(std::uint64_t offset) {
#ifdef _WIN32
        if (_fseeki64(file_, static_cast<__int64>(offset), SEEK_SET) != 0)
#else
        if (fseeko(file_, static_cast<off_t>(offset), SEEK_SET) != 0)
#endif
            throw Error(ErrorCode::Io, "member image seek failed");
    }
};

struct ManifestEntry {
    std::array<std::byte, 16> member_id{};
    std::uint64_t bytes = 0;
    bool operator==(const ManifestEntry&) const = default;
};
struct Header {
    std::array<std::byte, 16> pool_id{};
    std::array<std::byte, 16> member_id{};
    std::uint8_t index = 0;
    std::uint64_t bytes = 0;
    std::uint8_t member_count = 2;
    std::vector<ManifestEntry> manifest;
    bool encrypted = false;
    std::array<std::byte, kEncryptionSaltBytes> salt{};
    std::uint32_t iterations = 0;
};

struct V4IoFault {
    std::uint64_t crash_at = 0;
    bool after = false;
    bool trace = false;
};

// Test processes opt in through their own environment. The production mount
// never counts I/O or exits unless these POOLFS_TEST_V4_* variables are set.
const V4IoFault& v4_io_fault() {
    static const V4IoFault config = [] {
        V4IoFault result;
        const char* spec = std::getenv("POOLFS_TEST_V4_CRASH_AT");
        if (spec && *spec) {
            const std::string_view text(spec);
            std::string_view number;
            if (text.starts_with("before:")) number = text.substr(7);
            else if (text.starts_with("after:")) {
                result.after = true;
                number = text.substr(6);
            } else {
                throw Error(ErrorCode::InvalidArgument, "invalid v4 crash-test point");
            }
            const auto parsed = std::from_chars(number.data(),
                number.data() + number.size(), result.crash_at);
            if (number.empty() || parsed.ec != std::errc{} ||
                parsed.ptr != number.data() + number.size() || !result.crash_at)
                throw Error(ErrorCode::InvalidArgument, "invalid v4 crash-test index");
        }
        const char* trace = std::getenv("POOLFS_TEST_V4_TRACE_IO");
        result.trace = trace && std::strcmp(trace, "1") == 0;
        return result;
    }();
    return config;
}

std::uint64_t v4_io_before(const char* kind, std::uint64_t offset,
                           std::size_t length) {
    const auto& config = v4_io_fault();
    if (!config.crash_at && !config.trace) return 0;
    static std::atomic<std::uint64_t> next{0};
    const auto index = next.fetch_add(1, std::memory_order_relaxed) + 1;
    if (config.trace) {
        std::fprintf(stderr, "POOLFS_V4_IO %llu %s %llu %zu\n",
            static_cast<unsigned long long>(index), kind,
            static_cast<unsigned long long>(offset), length);
        std::fflush(stderr);
    }
    // _Exit simulates a process loss without C++ destructors or stdio flushes.
    if (config.crash_at == index && !config.after) std::_Exit(86);
    return index;
}

void v4_io_after(std::uint64_t index) {
    const auto& config = v4_io_fault();
    if (index && config.after && config.crash_at == index) std::_Exit(86);
}

class DiskMember {
public:
    explicit DiskMember(std::unique_ptr<MemberDevice> device, bool v4_test_io = false)
        : device_(std::move(device)), v4_test_io_(v4_test_io) {}
    Header header;
    std::uint64_t size() const { return device_->size(); }
    void read(std::uint64_t offset, std::byte* data, std::size_t length) {
        if (!cipher_ || !length ||
            (offset < 2 * kHeaderBytes &&
             length <= 2 * kHeaderBytes - offset)) {
            device_->read(offset, data, length);
            return;
        }
        if (offset < 2 * kHeaderBytes)
            throw Error(ErrorCode::InvalidArgument, "encrypted read crosses public headers");
        std::array<std::byte, kHeaderBytes> ciphertext{};
        std::array<std::byte, kHeaderBytes> plaintext{};
        struct WipePlaintext {
            std::array<std::byte, kHeaderBytes>& block;
            ~WipePlaintext() {
#ifdef _WIN32
                SecureZeroMemory(block.data(), block.size());
#else
                std::fill(block.begin(), block.end(), std::byte{0});
#endif
            }
        } wipe{plaintext};
        while (length) {
            const auto block_offset = offset & ~(kHeaderBytes - 1);
            const auto within = static_cast<std::size_t>(offset - block_offset);
            const auto amount = std::min(length, static_cast<std::size_t>(kHeaderBytes) - within);
            device_->read(block_offset, ciphertext.data(), ciphertext.size());
            if (std::all_of(ciphertext.begin(), ciphertext.end(),
                            [](std::byte value) { return value == std::byte{0}; }))
                plaintext.fill(std::byte{0});
            else
                cipher_->decrypt_unit(unit_number(block_offset), ciphertext, plaintext);
            std::copy_n(plaintext.data() + within, amount, data);
            offset += amount;
            data += amount;
            length -= amount;
        }
    }
    void write(std::uint64_t offset, const std::byte* data, std::size_t length) {
        const auto index = v4_test_io_ ? v4_io_before("write", offset, length) : 0;
        if (!cipher_ || !length ||
            (offset < 2 * kHeaderBytes &&
             length <= 2 * kHeaderBytes - offset)) {
            device_->write(offset, data, length);
        } else {
            if (offset < 2 * kHeaderBytes)
                throw Error(ErrorCode::InvalidArgument, "encrypted write crosses public headers");
            std::array<std::byte, kHeaderBytes> ciphertext{};
            std::array<std::byte, kHeaderBytes> plaintext{};
            struct WipePlaintext {
                std::array<std::byte, kHeaderBytes>& block;
                ~WipePlaintext() {
#ifdef _WIN32
                    SecureZeroMemory(block.data(), block.size());
#else
                    std::fill(block.begin(), block.end(), std::byte{0});
#endif
                }
            } wipe{plaintext};
            while (length) {
                const auto block_offset = offset & ~(kHeaderBytes - 1);
                const auto within = static_cast<std::size_t>(offset - block_offset);
                const auto amount = std::min(length, static_cast<std::size_t>(kHeaderBytes) - within);
                if (within || amount != kHeaderBytes) {
                    device_->read(block_offset, ciphertext.data(), ciphertext.size());
                    if (std::all_of(ciphertext.begin(), ciphertext.end(),
                                    [](std::byte value) { return value == std::byte{0}; }))
                        plaintext.fill(std::byte{0});
                    else
                        cipher_->decrypt_unit(unit_number(block_offset), ciphertext, plaintext);
                }
                std::copy_n(data, amount, plaintext.data() + within);
                cipher_->encrypt_unit(unit_number(block_offset), plaintext, ciphertext);
                device_->write(block_offset, ciphertext.data(), ciphertext.size());
                offset += amount;
                data += amount;
                length -= amount;
            }
        }
        if (v4_test_io_) v4_io_after(index);
    }
    void sync() {
        const auto index = v4_test_io_ ? v4_io_before("sync", 0, 0) : 0;
        device_->sync();
        if (v4_test_io_) v4_io_after(index);
    }
    void set_cipher(std::span<const std::byte, 64> xts_key) {
        if (!header.encrypted)
            throw Error(ErrorCode::InvalidArgument, "cannot encrypt a plaintext member");
        cipher_ = std::make_unique<crypto::XtsCipher>(xts_key);
    }
    void verify_header_mac(std::span<const std::byte> mac_key) const {
        if (!header.encrypted || raw_header_.size() != kHeaderBytes)
            throw Error(ErrorCode::Corrupt, "encrypted member header missing");
        auto canonical = raw_header_;
        const std::array<std::byte, kEncryptionMacBytes> expected = [&] {
            std::array<std::byte, kEncryptionMacBytes> value{};
            std::copy_n(canonical.begin() + kEncryptionMacOffset, value.size(), value.begin());
            return value;
        }();
        std::fill_n(canonical.begin() + 56, 4, std::byte{0});
        std::fill_n(canonical.begin() + kEncryptionMacOffset,
                    kEncryptionMacBytes, std::byte{0});
        const auto actual = crypto::hmac_sha256(mac_key, canonical);
        if (!crypto::constant_time_equal(expected, actual))
            throw Error(ErrorCode::AccessDenied, "invalid pool password or encrypted header");
    }
    void write_header(const Header& h, const Geometry& geometry,
                      std::span<const std::byte> mac_key = {}) {
        Bytes b(kHeaderBytes);
        for (std::size_t i = 0; i < geometry.header_magic.size(); ++i)
            b[i] = static_cast<std::byte>(geometry.header_magic[i]);
        Bytes fields;
        put32(fields, geometry.version);
        fields.insert(fields.end(), h.pool_id.begin(), h.pool_id.end());
        fields.insert(fields.end(), h.member_id.begin(), h.member_id.end());
        put8(fields, h.index);
        std::copy(fields.begin(), fields.end(), b.begin() + 8);
        // Fixed offsets: 8 version, 12 pool, 28 member, 44 index, 48 size, 56 checksum.
        for (int i = 0; i < 8; ++i) b[48 + i] = static_cast<std::byte>(h.bytes >> (8 * i));
        if (geometry.version >= 3) {
            if (h.member_count < (geometry.version == 6 ? 1 : 2) ||
                h.member_count > kMaximumV3Members ||
                h.manifest.size() != h.member_count || h.index >= h.member_count)
                throw Error(ErrorCode::InvalidArgument, "invalid v3 member manifest");
            b[45] = static_cast<std::byte>(h.member_count);
            if (geometry.version == 6)
                b[46] = static_cast<std::byte>(h.encrypted ?
                    kV6EncryptedLayoutRevision : kV6LayoutRevision);
            for (std::size_t member = 0; member < h.manifest.size(); ++member) {
                const auto base = kManifestOffset + member * kManifestEntryBytes;
                std::copy(h.manifest[member].member_id.begin(),
                          h.manifest[member].member_id.end(), b.begin() + base);
                for (int i = 0; i < 8; ++i)
                    b[base + 16 + i] = static_cast<std::byte>(h.manifest[member].bytes >> (8 * i));
            }
            if (h.encrypted) {
                if (geometry.version != 6 || mac_key.size() != kEncryptionMacBytes ||
                    h.iterations < 100000 || h.iterations > 10000000)
                    throw Error(ErrorCode::InvalidArgument, "invalid encrypted header parameters");
                std::copy(h.salt.begin(), h.salt.end(),
                          b.begin() + kEncryptionSaltOffset);
                overwrite32(b, kEncryptionIterationsOffset, h.iterations);
                const auto mac = crypto::hmac_sha256(mac_key, b);
                std::copy(mac.begin(), mac.end(), b.begin() + kEncryptionMacOffset);
            }
            overwrite32(b, 56, fnv1a(b.data(), b.size()));
        } else {
            overwrite32(b, 56, fnv1a(b.data(), 56));
        }
        write(0, b.data(), b.size());
        write(kHeaderBytes, b.data(), b.size());
        sync();
    }
    void load_header(const Geometry& geometry) {
        Bytes a(kHeaderBytes), b(kHeaderBytes);
        read(0, a.data(), a.size());
        read(kHeaderBytes, b.data(), b.size());
        const bool va = valid_header(a, geometry), vb = valid_header(b, geometry);
        if (!va && !vb) {
            if (geometry.version == 6) {
                const auto old_layout = [&](const Bytes& candidate) {
                    if (byte_at(candidate, 46) == kV6LayoutRevision ||
                        byte_at(candidate, 46) == kV6EncryptedLayoutRevision ||
                        u32_at(candidate, 8) != 6) return false;
                    for (std::size_t i = 0; i < geometry.header_magic.size(); ++i)
                        if (byte_at(candidate, i) !=
                            static_cast<std::uint8_t>(geometry.header_magic[i]))
                            return false;
                    Bytes checksum_source = candidate;
                    overwrite32(checksum_source, 56, 0);
                    return u32_at(candidate, 56) ==
                        fnv1a(checksum_source.data(), checksum_source.size());
                };
                if (old_layout(a) || old_layout(b))
                    throw Error(ErrorCode::Corrupt,
                        "unsupported earlier v6 member layout");
            }
            throw Error(ErrorCode::Corrupt, "member has no valid pool header");
        }
        if (va && vb && a != b) throw Error(ErrorCode::Corrupt, "member header copies disagree");
        const Bytes& src = va ? a : b;
        raw_header_ = src;
        for (int i = 0; i < 16; ++i) {
            header.pool_id[i] = src[12 + i];
            header.member_id[i] = src[28 + i];
        }
        header.index = byte_at(src, 44);
        header.bytes = u64_at(src, 48);
        if (geometry.version >= 3) {
            if (geometry.version == 6 &&
                byte_at(src, 46) == kV6EncryptedLayoutRevision) {
                header.encrypted = true;
                std::copy_n(src.begin() + kEncryptionSaltOffset,
                            header.salt.size(), header.salt.begin());
                header.iterations = u32_at(src, kEncryptionIterationsOffset);
            }
            header.member_count = byte_at(src, 45);
            header.manifest.clear();
            for (std::size_t member = 0; member < header.member_count; ++member) {
                ManifestEntry entry;
                const auto base = kManifestOffset + member * kManifestEntryBytes;
                std::copy_n(src.begin() + base, entry.member_id.size(), entry.member_id.begin());
                entry.bytes = u64_at(src, base + 16);
                const auto minimum = geometry.version == 6 ? Pool::kMinimumV6ImageBytes :
                    geometry.version == 5 ? Pool::kMinimumV5ImageBytes :
                    (geometry.version == 4 ? Pool::kMinimumV4ImageBytes : Pool::kMinimumRawBytes);
                if (entry.bytes < minimum ||
                    entry.bytes % geometry.io_block != 0)
                    throw Error(ErrorCode::Corrupt, "invalid member size in manifest");
                for (const auto& previous : header.manifest)
                    if (previous.member_id == entry.member_id)
                        throw Error(ErrorCode::Corrupt, "duplicate member identity in manifest");
                header.manifest.push_back(entry);
            }
            if (header.manifest[header.index].member_id != header.member_id ||
                header.manifest[header.index].bytes != header.bytes)
                throw Error(ErrorCode::Corrupt, "member does not match manifest");
        }
        const auto minimum = geometry.version == 1 ? Pool::kMinimumImageBytes :
            geometry.version == 6 ? Pool::kMinimumV6ImageBytes :
            geometry.version == 5 ? Pool::kMinimumV5ImageBytes :
            geometry.version == 4 ? Pool::kMinimumV4ImageBytes : Pool::kMinimumRawBytes;
        if (header.bytes < minimum || header.bytes != size() ||
            (geometry.version >= 2 && header.bytes % geometry.io_block != 0))
            throw Error(ErrorCode::Corrupt, "member size does not match header");
    }
private:
    std::unique_ptr<MemberDevice> device_;
    bool v4_test_io_ = false;
    Bytes raw_header_;
    std::unique_ptr<crypto::XtsCipher> cipher_;
    std::uint64_t unit_number(std::uint64_t offset) const {
        return (static_cast<std::uint64_t>(header.index) << 56) |
               (offset / kHeaderBytes);
    }
    static bool valid_header(const Bytes& b, const Geometry& geometry) {
        for (std::size_t i = 0; i < geometry.header_magic.size(); ++i)
            if (byte_at(b, i) != static_cast<std::uint8_t>(geometry.header_magic[i])) return false;
        if (u32_at(b, 8) != geometry.version) return false;
        if (geometry.version >= 3) {
            if (geometry.version == 6 && byte_at(b, 46) != kV6LayoutRevision &&
                byte_at(b, 46) != kV6EncryptedLayoutRevision) return false;
            if (geometry.version == 6 &&
                byte_at(b, 46) == kV6EncryptedLayoutRevision) {
                const auto iterations = u32_at(b, kEncryptionIterationsOffset);
                if (iterations < 100000 || iterations > 10000000 ||
                    std::all_of(b.begin() + kEncryptionSaltOffset,
                        b.begin() + kEncryptionSaltOffset + kEncryptionSaltBytes,
                        [](std::byte value) { return value == std::byte{0}; }))
                    return false;
            }
            const auto count = byte_at(b, 45);
            if (count < (geometry.version == 6 ? 1 : 2) ||
                count > kMaximumV3Members || byte_at(b, 44) >= count)
                return false;
            Bytes checksum_source = b;
            overwrite32(checksum_source, 56, 0);
            return u32_at(b, 56) == fnv1a(checksum_source.data(), checksum_source.size());
        }
        return byte_at(b, 44) <= 1 && u32_at(b, 56) == fnv1a(b.data(), 56);
    }
};

std::array<std::byte, 16> random_id() {
    std::array<std::byte, 16> id{};
    std::random_device rd;
    for (auto& b : id) b = static_cast<std::byte>(rd() & 0xffU);
    return id;
}
std::array<std::byte, 16> secure_random_id() {
    std::array<std::byte, 16> id{};
    crypto::random_bytes(id);
    return id;
}
struct Record {
    Bytes raw;
    Bytes payload;
};
struct Scan {
    std::vector<Record> records;
    bool invalid_tail = false;
};
Bytes encode_record(std::uint64_t sequence, const Bytes& payload, const Geometry& geometry) {
    if (payload.size() > std::numeric_limits<std::uint32_t>::max() - kRecordHeaderBytes)
        throw std::length_error("journal record too large");
    Bytes b;
    for (char c : geometry.record_magic) put8(b, static_cast<std::uint8_t>(c));
    put32(b, static_cast<std::uint32_t>(payload.size() + kRecordHeaderBytes));
    put64(b, sequence);
    put32(b, 0);
    b.insert(b.end(), payload.begin(), payload.end());
    if (geometry.version >= 2) {
        if (b.size() > geometry.io_block)
            throw Error(ErrorCode::JournalFull, "metadata record exceeds block size");
        b.resize(geometry.io_block, std::byte{0});
    }
    const auto crc = fnv1a(b.data(), b.size());
    overwrite32(b, 16, crc);
    return b;
}
Scan scan_journal(DiskMember& disk, const Geometry& geometry,
                  std::uint64_t start, std::uint64_t bytes,
                  std::uint64_t first_sequence) {
    Scan out;
    std::uint64_t offset = start;
    std::uint64_t next_seq = first_sequence;
    const auto slot = geometry.version >= 2 ? geometry.io_block : kRecordHeaderBytes;
    while (offset + slot <= start + bytes) {
        Bytes h(slot);
        disk.read(offset, h.data(), h.size());
        if (std::all_of(h.begin(), h.end(), [](std::byte b) { return b == std::byte{0}; }))
            break;
        bool magic = true;
        for (std::size_t i = 0; i < geometry.record_magic.size(); ++i)
            magic &= byte_at(h, i) == static_cast<std::uint8_t>(geometry.record_magic[i]);
        const auto length = u32_at(h, 4);
        const auto stored_bytes = geometry.version >= 2 ? geometry.io_block : length;
        if (!magic || length < kRecordHeaderBytes || length > bytes ||
            (geometry.version >= 2 && length > geometry.io_block) ||
            offset + stored_bytes > start + bytes ||
            u64_at(h, 8) != next_seq) {
            out.invalid_tail = true;
            break;
        }
        Bytes raw(stored_bytes);
        if (geometry.version >= 2) raw = std::move(h);
        else disk.read(offset, raw.data(), raw.size());
        const auto stored_crc = u32_at(raw, 16);
        overwrite32(raw, 16, 0);
        const auto actual_crc = fnv1a(raw.data(), raw.size());
        overwrite32(raw, 16, stored_crc);
        if (stored_crc != actual_crc ||
            (geometry.version >= 2 && !std::all_of(raw.begin() + length, raw.end(),
                [](std::byte b) { return b == std::byte{0}; }))) {
            out.invalid_tail = true;
            break;
        }
        Record record;
        record.payload.assign(raw.begin() + kRecordHeaderBytes, raw.begin() + length);
        record.raw = std::move(raw);
        out.records.push_back(std::move(record));
        offset += stored_bytes;
        ++next_seq;
    }
    return out;
}
Scan scan_journal(DiskMember& disk, const Geometry& geometry) {
    return scan_journal(disk, geometry, geometry.journal_offset, geometry.journal_bytes, 1);
}

struct V4Checkpoint {
    std::uint64_t generation = 0;
    std::uint64_t next_sequence = 0;
    std::uint8_t epoch = 0;
    Bytes payload;
};
constexpr std::array<char, 8> kV4CheckpointMagic{'P','F','S','C','P','0','0','4'};
constexpr std::array<char, 8> kV5CheckpointMagic{'P','F','S','C','P','0','0','5'};
constexpr std::array<char, 8> kV6CheckpointMagic{'P','F','S','C','P','0','0','6'};
Bytes empty_v4_snapshot(std::size_t members, const Geometry& geometry) {
    Bytes payload;
    for (char c : std::array<char, 4>{'S','N','P', static_cast<char>('0' + geometry.version)})
        put8(payload, static_cast<std::uint8_t>(c));
    put8(payload, static_cast<std::uint8_t>(members));
    for (std::size_t i = 0; i < members; ++i) put64(payload, geometry.data_offset);
    if (geometry.version == 6) {
        const auto now = current_filetime();
        put32(payload, kFileAttributeDirectory);
        for (int i = 0; i < 4; ++i) put64(payload, now);
        put64(payload, 2); // Root is file ID 1.
    }
    put32(payload, 0);
    if (geometry.version == 6) put32(payload, 0); // No active checksum pages yet.
    return payload;
}
Bytes v4_checkpoint_header(const V4Checkpoint& cp, const Geometry& geometry) {
    Bytes header(kRawV4.io_block);
    const auto& magic = geometry.version == 6 ? kV6CheckpointMagic :
        geometry.version == 5 ? kV5CheckpointMagic : kV4CheckpointMagic;
    for (std::size_t i = 0; i < magic.size(); ++i)
        header[i] = static_cast<std::byte>(magic[i]);
    Bytes fields;
    put64(fields, cp.generation);
    put64(fields, cp.next_sequence);
    put8(fields, cp.epoch);
    std::copy(fields.begin(), fields.end(), header.begin() + 8);
    const auto payload_hash = fnv1a64(cp.payload.data(), cp.payload.size());
    for (int i = 0; i < 8; ++i) {
        header[32 + i] = static_cast<std::byte>(cp.payload.size() >> (8 * i));
        header[40 + i] = static_cast<std::byte>(payload_hash >> (8 * i));
    }
    const auto checksum = fnv1a64(header.data(), header.size());
    for (int i = 0; i < 8; ++i) header[48 + i] = static_cast<std::byte>(checksum >> (8 * i));
    return header;
}
void write_v4_checkpoint(DiskMember& disk, std::uint8_t slot,
                         const V4Checkpoint& cp, const Geometry& geometry) {
    if (slot > 1 || cp.epoch > 1 || !cp.generation || !cp.next_sequence ||
        slot != (cp.generation - 1) % 2 ||
        cp.epoch != (cp.generation - 1) % 2 ||
        cp.payload.size() > kV4CheckpointSlotBytes - kRawV4.io_block)
        throw Error(ErrorCode::JournalFull, "v4 checkpoint metadata area is full");
    const auto offset = v4_checkpoint_offset(slot);
    const Bytes invalid(kRawV4.io_block);
    disk.write(offset, invalid.data(), invalid.size());
    disk.sync();
    disk.write(offset + kRawV4.io_block, cp.payload.data(), cp.payload.size());
    disk.sync();
    const auto header = v4_checkpoint_header(cp, geometry);
    disk.write(offset, header.data(), header.size());
    disk.sync();
}
std::optional<V4Checkpoint> read_v4_checkpoint(DiskMember& disk, std::uint8_t slot,
                                               const Geometry& geometry) {
    const auto offset = v4_checkpoint_offset(slot);
    Bytes header(kRawV4.io_block);
    disk.read(offset, header.data(), header.size());
    const auto& magic = geometry.version == 6 ? kV6CheckpointMagic :
        geometry.version == 5 ? kV5CheckpointMagic : kV4CheckpointMagic;
    for (std::size_t i = 0; i < magic.size(); ++i)
        if (byte_at(header, i) != static_cast<std::uint8_t>(magic[i]))
            return std::nullopt;
    const auto generation = u64_at(header, 8);
    const auto next_sequence = u64_at(header, 16);
    const auto epoch = byte_at(header, 24);
    const auto length = u64_at(header, 32);
    const auto payload_hash = u64_at(header, 40);
    const auto header_hash = u64_at(header, 48);
    for (int i = 0; i < 8; ++i) header[48 + i] = std::byte{0};
    if (!generation || !next_sequence || epoch > 1 ||
        slot != (generation - 1) % 2 || epoch != (generation - 1) % 2 ||
        length > kV4CheckpointSlotBytes - kRawV4.io_block ||
        header_hash != fnv1a64(header.data(), header.size())) return std::nullopt;
    V4Checkpoint cp;
    cp.generation = generation;
    cp.next_sequence = next_sequence;
    cp.epoch = epoch;
    cp.payload.resize(static_cast<std::size_t>(length));
    disk.read(offset + kRawV4.io_block, cp.payload.data(), cp.payload.size());
    if (payload_hash != fnv1a64(cp.payload.data(), cp.payload.size())) return std::nullopt;
    return cp;
}

std::vector<std::unique_ptr<RawVolume>> open_raw_v3(const std::vector<RawMemberSpec>& members) {
    if (members.size() < 2 || members.size() > kMaximumV3Members)
        throw Error(ErrorCode::InvalidArgument, "v3 requires 2 to 16 members");
    std::vector<std::unique_ptr<RawVolume>> volumes;
    volumes.reserve(members.size());
    std::uint64_t total_capacity = 0;
    for (const auto& spec : members) {
        if (spec.expected_partition_guid.empty())
            throw Error(ErrorCode::InvalidArgument, "v3 requires each expected partition GUID");
        auto volume = std::make_unique<RawVolume>(spec.volume_path, spec.expected_disk_guid,
                                                  spec.expected_partition_guid);
        const auto& info = volume->info();
        if (volume->size() < Pool::kMinimumRawBytes || volume->size() % kRawV3.io_block != 0 ||
            info.partition_guid.empty() || !info.partition_length_bytes ||
            info.partition_start_bytes > std::numeric_limits<std::uint64_t>::max() -
                                         info.partition_length_bytes)
            throw Error(ErrorCode::InvalidArgument, "v3 raw member has invalid geometry");
        const auto contribution = volume->size() - kRawV3.data_offset;
        if (contribution > std::numeric_limits<std::uint64_t>::max() - total_capacity)
            throw Error(ErrorCode::InvalidArgument, "v3 pool capacity exceeds addressable range");
        total_capacity += contribution;
        auto guid = info.partition_guid;
        std::transform(guid.begin(), guid.end(), guid.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        for (const auto& prior : volumes) {
            auto prior_guid = prior->info().partition_guid;
            std::transform(prior_guid.begin(), prior_guid.end(), prior_guid.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (guid == prior_guid)
                throw Error(ErrorCode::InvalidArgument, "v3 members have duplicate partition GUIDs");
            const auto& other = prior->info();
            if (info.disk_number == other.disk_number) {
                const auto end = info.partition_start_bytes + info.partition_length_bytes;
                const auto other_end = other.partition_start_bytes + other.partition_length_bytes;
                if (info.partition_start_bytes < other_end &&
                    other.partition_start_bytes < end)
                    throw Error(ErrorCode::InvalidArgument, "v3 raw member partitions overlap");
            }
        }
        volumes.push_back(std::move(volume));
    }
    return volumes;
}

} // namespace

struct Pool::Impl {
    struct Extent {
        std::uint64_t logical = 0;
        std::uint64_t physical = 0;
        std::uint32_t length = 0;
        std::uint8_t member = 0;
    };
    struct File {
        Entry entry;
        std::vector<Extent> extents;
        // V4 contains only live, non-overlapping logical ranges.
        std::map<std::uint64_t, Extent> indexed;
    };
    struct FreeRanges {
        std::map<std::uint64_t, std::uint64_t> by_start;
        std::set<std::pair<std::uint64_t, std::uint64_t>> by_size;
        std::uint64_t total = 0;
        void add(std::uint64_t start, std::uint64_t length) {
            if (!length) return;
            const auto added_length = length;
            auto next = by_start.lower_bound(start);
            if (next != by_start.end() && start + length > next->first)
                throw Error(ErrorCode::Corrupt, "overlapping free ranges");
            if (next != by_start.begin()) {
                auto prev = std::prev(next);
                if (prev->first + prev->second > start)
                    throw Error(ErrorCode::Corrupt, "overlapping free ranges");
                if (prev->first + prev->second == start) {
                    start = prev->first;
                    length += prev->second;
                    by_size.erase({prev->second, prev->first});
                    by_start.erase(prev);
                }
            }
            next = by_start.lower_bound(start);
            if (next != by_start.end() && start + length == next->first) {
                length += next->second;
                by_size.erase({next->second, next->first});
                by_start.erase(next);
            }
            by_start.emplace(start, length);
            by_size.emplace(length, start);
            total += added_length;
        }
        bool consume(std::uint64_t start, std::uint64_t length) {
            if (!length) return true;
            auto next = by_start.upper_bound(start);
            if (next == by_start.begin()) return false;
            auto it = std::prev(next);
            if (start < it->first || length > it->second ||
                start - it->first > it->second - length) return false;
            const auto old_start = it->first;
            const auto old_length = it->second;
            by_size.erase({old_length, old_start});
            by_start.erase(it);
            const auto left = start - old_start;
            const auto right = old_length - left - length;
            if (left) {
                by_start.emplace(old_start, left);
                by_size.emplace(left, old_start);
            }
            if (right) {
                by_start.emplace(start + length, right);
                by_size.emplace(right, start + length);
            }
            total -= length;
            return true;
        }
        std::optional<std::pair<std::uint64_t, std::uint64_t>> fit(std::uint64_t length) const {
            auto it = by_size.lower_bound({length, 0});
            if (it == by_size.end()) return std::nullopt;
            return std::pair{it->second, it->first};
        }
        std::optional<std::pair<std::uint64_t, std::uint64_t>> largest() const {
            if (by_size.empty()) return std::nullopt;
            auto it = std::prev(by_size.end());
            return std::pair{it->second, it->first};
        }
    };
    struct V6Root {
        std::uint8_t slot = 0;
        std::uint64_t hash = 0;
    };
    std::vector<std::unique_ptr<DiskMember>> disks;
    std::map<std::string, File> files;
    std::map<std::uint64_t, std::string> id_paths;
    Entry root_entry{"", 0, 255, 1, true, kFileAttributeDirectory};
    std::uint64_t next_file_id = 2;
    Geometry geometry;
    std::vector<std::uint64_t> data_tail;
    std::vector<FreeRanges> free_ranges;
    std::vector<std::map<std::uint64_t, V6Root>> v6_roots;
    // The selected durable checkpoint is a rollback root. Its data extents
    // and checksum-page slots remain protected until the next checkpoint is
    // fully written and the previous one retired.
    std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> v6_checkpoint_extents;
    std::vector<std::map<std::uint64_t, V6Root>> v6_checkpoint_roots;
    std::uint64_t journal_tail = 0;
    std::uint64_t next_sequence = 1;
    std::uint64_t checkpoint_generation = 0;
    std::uint8_t active_epoch = 0;
    bool writable = true;
    bool incomplete_tail = false;
    mutable std::mutex mutex;
    std::uint64_t data_end(std::size_t member) const {
        const auto bytes = disks[member]->header.bytes;
        return geometry.version == 6 ? v6_data_end(bytes) :
            geometry.version == 5 ? v5_data_end(bytes) : bytes;
    }
    std::uint64_t checksum_offset(std::size_t member, std::uint64_t physical) const {
        return v5_checksum_start(disks[member]->header.bytes) +
            ((physical - geometry.data_offset) / geometry.io_block) * 8;
    }

    Impl(const std::filesystem::path& first, const std::filesystem::path& second,
         const Geometry& format) : geometry(format) {
        auto a = std::make_unique<DiskMember>(std::make_unique<DiskImage>(first));
        auto b = std::make_unique<DiskMember>(std::make_unique<DiskImage>(second));
        if (std::filesystem::equivalent(first, second))
            throw Error(ErrorCode::InvalidArgument, "pool members must be different images");
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.push_back(std::move(a));
        opened.push_back(std::move(b));
        open_members(std::move(opened));
    }
    Impl(const RawMemberSpec& first, const RawMemberSpec& second)
        : geometry(kRawV2) {
        auto a = std::make_unique<RawVolume>(first.volume_path, first.expected_disk_guid);
        auto b = std::make_unique<RawVolume>(second.volume_path, second.expected_disk_guid);
        if (a->info().disk_number == b->info().disk_number)
            throw Error(ErrorCode::InvalidArgument, "raw members must be on different disks");
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.push_back(std::make_unique<DiskMember>(std::move(a)));
        opened.push_back(std::make_unique<DiskMember>(std::move(b)));
        open_members(std::move(opened));
    }
    Impl(Pool::V3ImageTag, const std::vector<std::filesystem::path>& members)
        : geometry(kRawV3) {
        if (members.size() < 2 || members.size() > kMaximumV3Members)
            throw Error(ErrorCode::InvalidArgument, "v3 requires 2 to 16 members");
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.reserve(members.size());
        for (const auto& path : members)
            opened.push_back(std::make_unique<DiskMember>(std::make_unique<DiskImage>(path)));
        open_members(std::move(opened));
    }
    Impl(Pool::V4ImageTag, const std::vector<std::filesystem::path>& members)
        : geometry(kRawV4) {
        if (members.size() < 2 || members.size() > kMaximumV3Members)
            throw Error(ErrorCode::InvalidArgument, "v4 requires 2 to 16 members");
        for (const auto& path : members) {
            auto undo = std::filesystem::absolute(path).lexically_normal();
            undo += L".poolfs-check-undo";
            if (std::filesystem::exists(undo))
                throw Error(ErrorCode::Corrupt,
                    "v4 repair is incomplete; run poolfs-check offline before mounting");
        }
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.reserve(members.size());
        for (const auto& path : members)
            opened.push_back(std::make_unique<DiskMember>(
                std::make_unique<DiskImage>(path), true));
        open_members(std::move(opened));
    }
    Impl(Pool::V5ImageTag, const std::vector<std::filesystem::path>& members,
         bool read_only) : geometry(kRawV5) {
        if (members.size() < 2 || members.size() > kMaximumV3Members)
            throw Error(ErrorCode::InvalidArgument, "v5 requires 2 to 16 members");
        writable = !read_only;
        for (const auto& path : members) {
            auto undo = std::filesystem::absolute(path).lexically_normal();
            undo += L".poolfs-check-undo";
            if (std::filesystem::exists(undo))
                throw Error(ErrorCode::Corrupt, "member has a pending repair sidecar");
        }
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.reserve(members.size());
        for (const auto& path : members)
            opened.push_back(std::make_unique<DiskMember>(
                std::make_unique<DiskImage>(path, false, read_only), !read_only));
        open_members(std::move(opened));
    }
    Impl(Pool::V6ImageTag, const std::vector<std::filesystem::path>& members,
         bool read_only, std::string_view password = {},
         bool require_encrypted = false) : geometry(kRawV6) {
        if (members.empty() || members.size() > kMaximumV3Members)
            throw Error(ErrorCode::InvalidArgument, "v6 requires 1 to 16 members");
        writable = !read_only;
        for (const auto& path : members) {
            auto undo = std::filesystem::absolute(path).lexically_normal();
            undo += L".poolfs-check-undo";
            if (std::filesystem::exists(undo))
                throw Error(ErrorCode::Corrupt, "member has a pending repair sidecar");
        }
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.reserve(members.size());
        for (const auto& path : members)
            opened.push_back(std::make_unique<DiskMember>(
                std::make_unique<DiskImage>(path, false, read_only), !read_only));
        open_members(std::move(opened), password, require_encrypted);
    }
    Impl(Pool::RawV3Tag, const std::vector<RawMemberSpec>& members)
        : geometry(kRawV3) {
        auto raw = open_raw_v3(members);
        std::vector<std::unique_ptr<DiskMember>> opened;
        opened.reserve(raw.size());
        for (auto& volume : raw)
            opened.push_back(std::make_unique<DiskMember>(std::move(volume)));
        open_members(std::move(opened));
    }
    void open_members(std::vector<std::unique_ptr<DiskMember>> opened,
                      std::string_view password = {},
                      bool require_encrypted = false) {
        const auto count = opened.size();
        if (count < (geometry.version == 6 ? 1 : 2) ||
            count > kMaximumV3Members ||
            (geometry.version < 3 && count != 2))
            throw Error(ErrorCode::InvalidArgument, "invalid member count");
        data_tail.assign(count, geometry.data_offset);
        v6_roots.resize(count);
        journal_tail = geometry.journal_offset;
        disks.resize(count);
        for (auto& disk : opened) disk->load_header(geometry);
        const Header reference = opened.front()->header;
        for (auto& disk : opened) {
            const auto& h = disk->header;
            if (h.index >= count || disks[h.index] ||
                h.pool_id != reference.pool_id ||
                (geometry.version >= 3 &&
                 (h.member_count != count || h.manifest != reference.manifest)))
                throw Error(ErrorCode::Corrupt, "members do not match pool manifest");
            disks[h.index] = std::move(disk);
        }
        for (std::size_t i = 0; i < count; ++i) {
            if (!disks[i]) throw Error(ErrorCode::Corrupt, "pool member is missing");
            if (geometry.version >= 3) {
                const auto& expected = disks[0]->header.manifest[i];
                if (disks[i]->header.member_id != expected.member_id ||
                    disks[i]->header.bytes != expected.bytes)
                    throw Error(ErrorCode::Corrupt, "member differs from pool manifest");
            }
        }
        if (geometry.version == 6) {
            for (const auto& disk : disks) {
                const auto& member = disk->header;
                if (member.encrypted != reference.encrypted ||
                    (member.encrypted && (member.salt != reference.salt ||
                                          member.iterations != reference.iterations)))
                    throw Error(ErrorCode::Corrupt, "pool encryption parameters disagree");
            }
            if (reference.encrypted) {
                if (password.empty())
                    throw Error(ErrorCode::AccessDenied, "encrypted pool requires a password");
                auto keys = crypto::derive_keys(password, reference.salt,
                                                reference.iterations);
                for (auto& disk : disks) disk->verify_header_mac(keys.mac_key());
                for (auto& disk : disks) disk->set_cipher(keys.xts_key());
            } else if (require_encrypted) {
                throw Error(ErrorCode::AccessDenied,
                    "this pool is not encrypted; refusing password unlock");
            }
        }
        if (geometry.version >= 3) {
            std::uint64_t total_capacity = 0;
            for (const auto& disk : disks) {
                const auto contribution = (geometry.version == 6 ?
                    v6_data_end(disk->header.bytes) : geometry.version == 5 ?
                    v5_data_end(disk->header.bytes) : disk->header.bytes) - geometry.data_offset;
                if (contribution > std::numeric_limits<std::uint64_t>::max() - total_capacity)
                    throw Error(ErrorCode::Corrupt, "v3 pool capacity exceeds addressable range");
                total_capacity += contribution;
            }
        }
        if (geometry.version < 3 && disks[0]->header.member_id == disks[1]->header.member_id)
            throw Error(ErrorCode::Corrupt, "members have duplicate identity");
        if (geometry.version >= 4) {
            open_v4();
            return;
        }
        std::vector<Scan> scans;
        scans.reserve(count);
        for (auto& disk : disks) scans.push_back(scan_journal(*disk, geometry));
        std::size_t n = scans.front().records.size();
        for (const auto& scan : scans) n = std::min(n, scan.records.size());
        std::size_t common = 0;
        while (common < n) {
            bool same = true;
            for (std::size_t i = 1; i < count; ++i)
                same &= scans[i].records[common].raw == scans[0].records[common].raw;
            if (!same) break;
            ++common;
        }
        for (const auto& scan : scans)
            incomplete_tail |= scan.invalid_tail || common != scan.records.size();
        // A v2 tail mismatch could be later damage to an acknowledged record,
        // not merely an interrupted write. Do not overwrite either tail until
        // an offline checker has established which copy is authoritative.
        if (geometry.version >= 2 && incomplete_tail) writable = false;
        for (std::size_t i = 0; i < common; ++i) {
            apply(scans[0].records[i].payload);
            journal_tail += scans[0].records[i].raw.size();
            ++next_sequence;
        }
    }

    void open_v4() {
        std::optional<V4Checkpoint> chosen;
        for (std::uint8_t slot = 0; slot < 2; ++slot) {
            auto candidate = read_v4_checkpoint(*disks[0], slot, geometry);
            if (!candidate) continue;
            bool common = true;
            for (std::size_t i = 1; i < disks.size(); ++i) {
                auto copy = read_v4_checkpoint(*disks[i], slot, geometry);
                if (!copy || copy->generation != candidate->generation ||
                    copy->next_sequence != candidate->next_sequence ||
                    copy->epoch != candidate->epoch || copy->payload != candidate->payload) {
                    common = false;
                    break;
                }
            }
            if (common && (!chosen || candidate->generation > chosen->generation))
                chosen = std::move(candidate);
        }
        if (!chosen) throw Error(ErrorCode::Corrupt, "v4 has no common valid checkpoint");
        checkpoint_generation = chosen->generation;
        active_epoch = chosen->epoch;
        next_sequence = chosen->next_sequence;
        load_v4_snapshot(chosen->payload);
        if (geometry.version == 6) capture_v6_checkpoint();
        journal_tail = v4_epoch_offset(active_epoch);
        std::vector<Scan> scans;
        scans.reserve(disks.size());
        for (auto& disk : disks)
            scans.push_back(scan_journal(*disk, geometry, journal_tail,
                                         kV4EpochBytes, next_sequence));
        std::size_t common_count = scans[0].records.size();
        std::size_t maximum_count = common_count;
        for (const auto& scan : scans) {
            if (scan.invalid_tail)
                throw Error(ErrorCode::Corrupt, "v4 journal contains a damaged record");
            common_count = std::min(common_count, scan.records.size());
            maximum_count = std::max(maximum_count, scan.records.size());
        }
        if (maximum_count - common_count > 1)
            throw Error(ErrorCode::Corrupt, "v4 journal copies have different histories");
        for (std::size_t j = 0; j < maximum_count; ++j) {
            const Bytes* expected = nullptr;
            for (const auto& scan : scans) {
                if (j >= scan.records.size()) continue;
                if (expected && scan.records[j].raw != *expected)
                    throw Error(ErrorCode::Corrupt, "v4 journal copies disagree");
                expected = &scan.records[j].raw;
            }
        }
        // One complete record on only a subset of members is a possible
        // interrupted mirrored commit. Its data was copy-on-write into blocks
        // free in the common prefix, so that prefix remains safe to read.
        if (maximum_count != common_count) {
            incomplete_tail = true;
            writable = false;
        }
        for (std::size_t j = 0; j < common_count; ++j) {
            apply(scans[0].records[j].payload);
            journal_tail += geometry.io_block;
            ++next_sequence;
        }
        if (writable) {
            // A crash may have interrupted retirement after every member got
            // the new checkpoint. Retire the inactive root before any new
            // allocation can reuse blocks it still names.
            const auto inactive_slot = static_cast<std::uint8_t>(checkpoint_generation % 2);
            const Bytes invalid(geometry.io_block);
            for (auto& disk : disks) {
                Bytes header(geometry.io_block);
                disk->read(v4_checkpoint_offset(inactive_slot), header.data(), header.size());
                if (header != invalid) {
                    disk->write(v4_checkpoint_offset(inactive_slot), invalid.data(), invalid.size());
                    disk->sync();
                }
            }
        }
    }

    File& find(const std::string& name) {
        auto it = files.find(name);
        if (it == files.end()) throw Error(ErrorCode::NotFound, "file not found: " + name);
        return it->second;
    }
    const File& find(const std::string& name) const {
        auto it = files.find(name);
        if (it == files.end()) throw Error(ErrorCode::NotFound, "file not found: " + name);
        return it->second;
    }
    Entry& find_by_id(std::uint64_t id) {
        if (id == 1) return root_entry;
        const auto it = id_paths.find(id);
        if (it == id_paths.end()) throw Error(ErrorCode::NotFound, "file ID not found");
        return find(it->second).entry;
    }
    void require_parent_directory(const std::string& path) const {
        const auto parent = parent_path(path);
        if (!parent.empty() && !find(parent).entry.is_directory)
            throw Error(ErrorCode::InvalidArgument, "parent is not a directory");
    }
    bool directory_empty(const std::string& path) const {
        const auto prefix = path.empty() ? std::string() : path + '/';
        for (const auto& [name, file] : files) {
            (void)file;
            if (parent_path(name) == path) return false;
            if (!prefix.empty() && name.starts_with(prefix)) return false;
        }
        return true;
    }
    void touch_parent(const std::string& path, std::uint64_t time) {
        const auto parent = parent_path(path);
        auto& entry = parent.empty() ? root_entry : find(parent).entry;
        entry.last_write_time = time;
        entry.change_time = time;
    }
    void capture_v6_checkpoint() {
        v6_checkpoint_extents.clear();
        v6_checkpoint_extents.resize(disks.size());
        v6_checkpoint_roots = v6_roots;
        for (const auto& [name, file] : files)
            for (const auto& [logical, extent] : file.indexed)
                v6_checkpoint_extents[extent.member].emplace_back(
                    extent.physical,
                    extent.physical + allocation_bytes(extent.length, geometry));
        for (auto& ranges : v6_checkpoint_extents) {
            std::sort(ranges.begin(), ranges.end());
            std::size_t write = 0;
            for (const auto& range : ranges) {
                if (write && range.first <= ranges[write - 1].second)
                    ranges[write - 1].second = std::max(ranges[write - 1].second,
                                                        range.second);
                else
                    ranges[write++] = range;
            }
            ranges.resize(write);
        }
    }
    Bytes v4_snapshot() const {
        Bytes out;
        for (char c : std::array<char, 4>{'S','N','P', static_cast<char>('0' + geometry.version)})
            put8(out, static_cast<std::uint8_t>(c));
        put8(out, static_cast<std::uint8_t>(disks.size()));
        for (const auto tail : data_tail) put64(out, tail);
        if (geometry.version == 6) {
            put32(out, root_entry.attributes);
            put64(out, root_entry.creation_time);
            put64(out, root_entry.last_access_time);
            put64(out, root_entry.last_write_time);
            put64(out, root_entry.change_time);
            put64(out, next_file_id);
        }
        if (files.size() > std::numeric_limits<std::uint32_t>::max())
            throw Error(ErrorCode::JournalFull, "v4 checkpoint has too many files");
        put32(out, static_cast<std::uint32_t>(files.size()));
        for (const auto& [name, file] : files) {
            put_name(out, name);
            if (geometry.version == 6) {
                put64(out, file.entry.id);
                put8(out, file.entry.is_directory ? 1 : 0);
                put32(out, file.entry.attributes);
                put64(out, file.entry.creation_time);
                put64(out, file.entry.last_access_time);
                put64(out, file.entry.last_write_time);
                put64(out, file.entry.change_time);
            }
            put64(out, file.entry.size);
            if (file.indexed.size() > std::numeric_limits<std::uint32_t>::max())
                throw Error(ErrorCode::JournalFull, "v4 checkpoint has too many extents");
            put32(out, static_cast<std::uint32_t>(file.indexed.size()));
            for (const auto& [logical, e] : file.indexed) {
                put64(out, logical);
                put64(out, e.physical);
                put32(out, e.length);
                put8(out, e.member);
            }
            if (out.size() > kV4CheckpointSlotBytes - kRawV4.io_block)
                throw Error(ErrorCode::JournalFull, "v4 checkpoint metadata area is full");
        }
        if (geometry.version == 6) {
            std::uint64_t count = 0;
            for (const auto& roots : v6_roots) count += roots.size();
            if (count > std::numeric_limits<std::uint32_t>::max())
                throw Error(ErrorCode::JournalFull, "v6 has too many checksum groups");
            put32(out, static_cast<std::uint32_t>(count));
            for (std::size_t member = 0; member < v6_roots.size(); ++member)
                for (const auto& [group, root] : v6_roots[member]) {
                    put8(out, static_cast<std::uint8_t>(member));
                    put64(out, group);
                    put8(out, root.slot);
                    put64(out, root.hash);
                }
            if (out.size() > kV4CheckpointSlotBytes - kRawV4.io_block)
                throw Error(ErrorCode::JournalFull, "v6 checkpoint checksum map is full");
        }
        return out;
    }
    void rebuild_v4_free_ranges() {
        free_ranges.clear();
        free_ranges.resize(disks.size());
        std::vector<std::vector<std::pair<std::uint64_t, std::uint64_t>>> used(disks.size());
        for (const auto& [name, file] : files)
            for (const auto& [logical, e] : file.indexed)
                used[e.member].emplace_back(e.physical,
                    e.physical + allocation_bytes(e.length, geometry));
        for (std::size_t member = 0; member < disks.size(); ++member) {
            auto& ranges = used[member];
            std::sort(ranges.begin(), ranges.end());
            auto cursor = geometry.data_offset;
            for (const auto& [begin, end] : ranges) {
                if (begin < cursor || end > data_tail[member])
                    throw Error(ErrorCode::Corrupt, "v4 checkpoint has overlapping data blocks");
                free_ranges[member].add(cursor, begin - cursor);
                cursor = end;
            }
            free_ranges[member].add(cursor, data_tail[member] - cursor);
        }
    }
    void load_v4_snapshot(const Bytes& payload) {
        Reader r{payload};
        for (char c : std::array<char, 4>{'S','N','P', static_cast<char>('0' + geometry.version)})
            if (r.get8() != static_cast<std::uint8_t>(c))
                throw Error(ErrorCode::Corrupt, "invalid v4 checkpoint payload");
        if (r.get8() != disks.size())
            throw Error(ErrorCode::Corrupt, "v4 checkpoint member count differs");
        data_tail.clear();
        for (std::size_t member = 0; member < disks.size(); ++member) {
            const auto tail = r.get64();
            if (tail < geometry.data_offset || tail > data_end(member) ||
                tail % geometry.io_block != 0)
                throw Error(ErrorCode::Corrupt, "invalid v4 checkpoint data tail");
            data_tail.push_back(tail);
        }
        if (geometry.version == 6) {
            root_entry.attributes = r.get32();
            root_entry.creation_time = r.get64();
            root_entry.last_access_time = r.get64();
            root_entry.last_write_time = r.get64();
            root_entry.change_time = r.get64();
            next_file_id = r.get64();
            if (!valid_attributes(root_entry.attributes, true) ||
                !root_entry.creation_time || next_file_id < 2)
                throw Error(ErrorCode::Corrupt, "invalid v6 root metadata");
        }
        const auto file_count = r.get32();
        files.clear();
        id_paths.clear();
        for (std::uint32_t i = 0; i < file_count; ++i) {
            const auto name = r.name(geometry.version == 6);
            File file;
            if (geometry.version == 6) {
                file.entry.name = name;
                file.entry.member = 255;
                file.entry.id = r.get64();
                const auto directory = r.get8();
                if (directory > 1) throw Error(ErrorCode::Corrupt, "invalid v6 entry type");
                file.entry.is_directory = directory != 0;
                file.entry.attributes = r.get32();
                file.entry.creation_time = r.get64();
                file.entry.last_access_time = r.get64();
                file.entry.last_write_time = r.get64();
                file.entry.change_time = r.get64();
                file.entry.size = r.get64();
                if (file.entry.id < 2 || file.entry.id >= next_file_id ||
                    !file.entry.creation_time ||
                    !valid_attributes(file.entry.attributes, file.entry.is_directory) ||
                    (file.entry.is_directory && file.entry.size != 0) ||
                    !id_paths.emplace(file.entry.id, name).second)
                    throw Error(ErrorCode::Corrupt, "invalid v6 entry metadata");
            } else {
                file.entry = Entry{name, r.get64(), 255};
            }
            const auto extent_count = r.get32();
            if (file.entry.is_directory && extent_count)
                throw Error(ErrorCode::Corrupt, "directory contains file extents");
            std::uint64_t previous_end = 0;
            for (std::uint32_t j = 0; j < extent_count; ++j) {
                Extent e;
                e.logical = r.get64();
                e.physical = r.get64();
                e.length = r.get32();
                e.member = r.get8();
                if (e.member >= disks.size() || !e.length ||
                    e.logical % geometry.io_block != 0 ||
                    e.physical % geometry.io_block != 0 ||
                    e.logical < previous_end ||
                    e.logical >= file.entry.size ||
                    e.logical > std::numeric_limits<std::uint64_t>::max() - e.length ||
                    (e.logical + e.length > file.entry.size &&
                     e.logical + e.length - file.entry.size >= geometry.io_block) ||
                    e.physical < geometry.data_offset ||
                    e.physical > data_tail[e.member] ||
                    allocation_bytes(e.length, geometry) > data_tail[e.member] - e.physical)
                    throw Error(ErrorCode::Corrupt, "invalid v4 checkpoint extent");
                previous_end = e.logical + e.length;
                file.indexed.emplace(e.logical, e);
            }
            if (!files.emplace(name, std::move(file)).second)
                throw Error(ErrorCode::Corrupt, "duplicate file in v4 checkpoint");
        }
        if (geometry.version == 6)
            for (const auto& [name, file] : files) {
                (void)file;
                require_parent_directory(name);
            }
        if (geometry.version == 6) {
            for (auto& roots : v6_roots) roots.clear();
            const auto count = r.get32();
            for (std::uint32_t i = 0; i < count; ++i) {
                const auto member = r.get8();
                const auto group = r.get64();
                const auto slot = r.get8();
                const auto hash = r.get64();
                if (member >= disks.size() ||
                    group >= v6_groups(v6_data_blocks(disks[member]->header.bytes)) ||
                    slot > 1 || !v6_roots[member].emplace(group, V6Root{slot, hash}).second)
                    throw Error(ErrorCode::Corrupt, "invalid v6 checkpoint checksum map");
            }
            for (const auto& [name, file] : files)
                for (const auto& [logical, e] : file.indexed) {
                    const auto first = (e.physical - geometry.data_offset) /
                        (kV6GroupBlocks * geometry.io_block);
                    const auto last = (e.physical +
                        allocation_bytes(e.length, geometry) - 1 - geometry.data_offset) /
                        (kV6GroupBlocks * geometry.io_block);
                    for (auto group = first; group <= last; ++group)
                        if (!v6_roots[e.member].contains(group))
                            throw Error(ErrorCode::Corrupt,
                                "v6 checkpoint is missing a live checksum root");
                }
        }
        r.done();
        rebuild_v4_free_ranges();
    }
    void v4_release_range(std::uint8_t member, std::uint64_t start,
                          std::uint64_t length) {
        if (geometry.version != 6 || v6_checkpoint_extents.empty()) {
            free_ranges[member].add(start, length);
            return;
        }
        const auto end = start + length;
        auto cursor = start;
        for (const auto& [protected_begin, protected_end] :
             v6_checkpoint_extents[member]) {
            if (protected_end <= cursor) continue;
            if (protected_begin >= end) break;
            if (cursor < protected_begin)
                free_ranges[member].add(cursor, protected_begin - cursor);
            cursor = std::min(end, std::max(cursor, protected_end));
            if (cursor == end) break;
        }
        if (cursor < end) free_ranges[member].add(cursor, end - cursor);
    }
    void v4_release(const Extent& e) {
        v4_release_range(e.member, e.physical,
            allocation_bytes(e.length, geometry));
    }
    void v4_coalesce(File& file, std::map<std::uint64_t, Extent>::iterator it) {
        if (it == file.indexed.end()) return;
        if (it != file.indexed.begin()) {
            auto prev = std::prev(it);
            if (prev->second.member == it->second.member &&
                prev->first + prev->second.length == it->first &&
                prev->second.physical + prev->second.length == it->second.physical &&
                prev->second.length <= std::numeric_limits<std::uint32_t>::max() - it->second.length) {
                prev->second.length += it->second.length;
                file.indexed.erase(it);
                it = prev;
            }
        }
        auto next = std::next(it);
        if (next != file.indexed.end() &&
            it->second.member == next->second.member &&
            it->first + it->second.length == next->first &&
            it->second.physical + it->second.length == next->second.physical &&
            it->second.length <= std::numeric_limits<std::uint32_t>::max() - next->second.length) {
            it->second.length += next->second.length;
            file.indexed.erase(next);
        }
    }
    void v4_splice(File& file, std::uint64_t begin, std::uint64_t end,
                   const std::vector<Extent>& replacement) {
        auto it = file.indexed.lower_bound(begin);
        if (it != file.indexed.begin()) {
            auto prev = std::prev(it);
            if (prev->first + prev->second.length > begin) it = prev;
        }
        std::vector<Extent> survivors;
        std::vector<std::pair<std::uint8_t, std::pair<std::uint64_t, std::uint64_t>>> released;
        while (it != file.indexed.end() && it->first < end) {
            const auto e = it->second;
            const auto e_end = e.logical + e.length;
            const auto cut_begin = std::max(begin, e.logical);
            const auto cut_end = std::min(end, e_end);
            if (e.logical < cut_begin)
                survivors.push_back(Extent{e.logical, e.physical,
                    static_cast<std::uint32_t>(cut_begin - e.logical), e.member});
            if (cut_end < e_end)
                survivors.push_back(Extent{cut_end, e.physical + cut_end - e.logical,
                    static_cast<std::uint32_t>(e_end - cut_end), e.member});
            const auto physical_begin = e.physical + cut_begin - e.logical;
            const auto physical_end = e.physical + cut_end - e.logical;
            released.push_back({e.member, {physical_begin,
                allocation_bytes(physical_end, geometry) - physical_begin}});
            it = file.indexed.erase(it);
        }
        for (const auto& e : survivors)
            file.indexed.emplace(e.logical, e);
        for (const auto& e : replacement) {
            auto inserted = file.indexed.emplace(e.logical, e);
            if (!inserted.second) throw Error(ErrorCode::Corrupt, "overlapping v4 write extent");
        }
        for (const auto& [member, range] : released)
            v4_release_range(member, range.first, range.second);
        for (const auto& e : replacement)
            v4_coalesce(file, file.indexed.find(e.logical));
    }
    void v5_read_blocks(std::uint8_t member, std::uint64_t physical,
                        std::span<std::byte> data,
                        std::vector<std::size_t>* damaged = nullptr) {
        const auto block = static_cast<std::size_t>(geometry.io_block);
        if (data.empty() || data.size() % block != 0)
            throw Error(ErrorCode::InvalidArgument, "invalid v5 checksum read length");
        disks[member]->read(physical, data.data(), data.size());
        Bytes checksums(data.size() / block * 8);
        disks[member]->read(checksum_offset(member, physical),
                             checksums.data(), checksums.size());
        for (std::size_t at = 0; at < data.size(); at += block) {
            std::uint64_t expected = 0;
            const auto cell = at / block * 8;
            for (int i = 0; i < 8; ++i)
                expected |= std::uint64_t(byte_at(checksums, cell + i)) << (i * 8);
            if (expected == crc64(data.data() + at, block)) continue;
            if (damaged) damaged->push_back(at / block);
            else throw Error(ErrorCode::Corrupt,
                "v5 data checksum mismatch on member " + std::to_string(member) +
                " at physical offset " + std::to_string(physical + at));
        }
    }
    void v6_read_blocks(std::uint8_t member, std::uint64_t physical,
                        std::span<std::byte> data,
                        std::vector<std::size_t>* damaged = nullptr) {
        const auto block = static_cast<std::size_t>(geometry.io_block);
        if (data.empty() || data.size() % block != 0)
            throw Error(ErrorCode::InvalidArgument, "invalid v6 checksum read length");
        const auto member_bytes = disks[member]->header.bytes;
        std::size_t consumed = 0;
        while (consumed < data.size()) {
            const auto physical_block = (physical + consumed - geometry.data_offset) / block;
            const auto group = physical_block / kV6GroupBlocks;
            const auto cell = physical_block % kV6GroupBlocks;
            const auto count = std::min<std::size_t>(
                data.size() / block - consumed / block,
                static_cast<std::size_t>(kV6GroupBlocks - cell));
            const auto root = v6_roots[member].find(group);
            Bytes page(block);
            bool valid_page = root != v6_roots[member].end();
            if (valid_page) {
                disks[member]->read(v6_checksum_page(member_bytes, group, root->second.slot),
                                     page.data(), page.size());
                valid_page = crc64(page.data(), page.size()) == root->second.hash;
            }
            if (!valid_page) {
                if (!damaged)
                    throw Error(ErrorCode::Corrupt,
                        "v6 checksum page is missing or damaged on member " +
                        std::to_string(member) + " group " + std::to_string(group));
                for (std::size_t i = 0; i < count; ++i)
                    damaged->push_back(consumed / block + i);
                consumed += count * block;
                continue;
            }
            disks[member]->read(physical + consumed, data.data() + consumed, count * block);
            for (std::size_t i = 0; i < count; ++i) {
                const auto expected = u64_at(page, static_cast<std::size_t>(cell + i) * 8);
                if (expected == crc64(data.data() + consumed + i * block, block)) continue;
                if (damaged) damaged->push_back(consumed / block + i);
                else throw Error(ErrorCode::Corrupt,
                    "v6 data checksum mismatch on member " + std::to_string(member) +
                    " at physical offset " + std::to_string(physical + consumed + i * block));
            }
            consumed += count * block;
        }
    }
    struct V6PageWrite {
        std::uint8_t member = 0;
        std::uint64_t group = 0;
        std::uint8_t slot = 0;
        std::uint64_t hash = 0;
        Bytes page;
    };
    bool v6_group_has_live_data(std::uint8_t member, std::uint64_t group) const {
        const auto begin = geometry.data_offset +
            group * kV6GroupBlocks * geometry.io_block;
        const auto end = std::min(begin + kV6GroupBlocks * geometry.io_block,
                                  data_end(member));
        for (const auto& [name, file] : files)
            for (const auto& [logical, extent] : file.indexed)
                if (extent.member == member && extent.physical < end &&
                    extent.physical + allocation_bytes(extent.length, geometry) > begin)
                    return true;
        return false;
    }
    std::vector<V6PageWrite> v6_plan_pages(const std::vector<Extent>& planned,
                                           std::span<const std::byte> aligned,
                                           std::uint64_t logical) {
        std::map<std::pair<std::uint8_t, std::uint64_t>, V6PageWrite> pending;
        const auto block = static_cast<std::uint64_t>(geometry.io_block);
        for (const auto& e : planned) {
            for (std::uint64_t at = 0; at < e.length; at += block) {
                const auto physical_block = (e.physical + at - geometry.data_offset) / block;
                const auto group = physical_block / kV6GroupBlocks;
                const auto cell = physical_block % kV6GroupBlocks;
                const auto key = std::pair{e.member, group};
                auto it = pending.find(key);
                if (it == pending.end()) {
                    V6PageWrite next;
                    next.member = e.member;
                    next.group = group;
                    next.page.resize(static_cast<std::size_t>(block));
                    const auto current = v6_roots[e.member].find(group);
                    if (current != v6_roots[e.member].end()) {
                        next.slot = static_cast<std::uint8_t>(1 - current->second.slot);
                        disks[e.member]->read(v6_checksum_page(
                            disks[e.member]->header.bytes, group, current->second.slot),
                            next.page.data(), next.page.size());
                        if (crc64(next.page.data(), next.page.size()) != current->second.hash) {
                            if (v6_group_has_live_data(e.member, group))
                                throw Error(ErrorCode::Corrupt,
                                    "v6 active checksum page is damaged on member " +
                                    std::to_string(e.member) + " group " + std::to_string(group));
                            // No current extent can use a checksum from this
                            // group. A later journal record can replace its
                            // obsolete page without trusting damaged cells.
                            std::fill(next.page.begin(), next.page.end(), std::byte{0});
                        }
                    }
                    it = pending.emplace(key, std::move(next)).first;
                }
                const auto source = aligned.data() + (e.logical - logical) + at;
                const auto hash = crc64(source, static_cast<std::size_t>(block));
                for (int byte = 0; byte < 8; ++byte)
                    it->second.page[static_cast<std::size_t>(cell) * 8 + byte] =
                        static_cast<std::byte>(hash >> (8 * byte));
            }
        }
        if (pending.size() > std::numeric_limits<std::uint8_t>::max())
            throw Error(ErrorCode::JournalFull, "v6 write touches too many checksum groups");
        std::vector<V6PageWrite> result;
        result.reserve(pending.size());
        for (auto& [key, page] : pending) {
            page.hash = crc64(page.page.data(), page.page.size());
            result.push_back(std::move(page));
        }
        return result;
    }
    void protect_v6_checkpoint_pages(const std::vector<V6PageWrite>& pages) {
        bool conflict = false;
        for (const auto& page : pages) {
            const auto& roots = v6_checkpoint_roots[page.member];
            const auto it = roots.find(page.group);
            if (it != roots.end() && it->second.slot == page.slot) {
                conflict = true;
                break;
            }
        }
        if (!conflict) return;
        // Publish the current state as the sole durable rollback root before
        // overwriting a page still named by the previous checkpoint.
        try {
            rotate_v4_checkpoint();
        } catch (const Error& error) {
            if (error.code() != ErrorCode::JournalFull) writable = false;
            throw;
        } catch (...) {
            writable = false;
            throw;
        }
        for (const auto& page : pages) {
            const auto& roots = v6_checkpoint_roots[page.member];
            const auto it = roots.find(page.group);
            if (it != roots.end() && it->second.slot == page.slot)
                throw Error(ErrorCode::Corrupt,
                    "v6 checksum page is still selected by the checkpoint");
        }
    }
    void v4_read_live(const File& file, std::uint64_t offset,
                      std::span<std::byte> destination) {
        std::fill(destination.begin(), destination.end(), std::byte{0});
        if (offset >= file.entry.size || destination.empty()) return;
        const auto end = offset + std::min<std::uint64_t>(destination.size(),
                                                         file.entry.size - offset);
        auto it = file.indexed.upper_bound(offset);
        if (it != file.indexed.begin()) --it;
        for (; it != file.indexed.end() && it->first < end; ++it) {
            const auto& e = it->second;
            const auto begin = std::max(offset, e.logical);
            const auto stop = std::min(end, e.logical + e.length);
            if (begin >= stop) continue;
            if (geometry.version >= 5) {
                const auto block = static_cast<std::uint64_t>(geometry.io_block);
                for (auto logical = begin / block * block; logical < stop;) {
                    constexpr std::size_t kMaximumBatchBlocks = 256;
                    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                        kMaximumBatchBlocks, (stop - logical + block - 1) / block));
                    const auto copy_begin = std::max(begin, logical);
                    const auto copy_end = std::min(stop, logical + count * block);
                    if (copy_begin == logical && copy_end == logical + count * block) {
                        auto target = destination.subspan(
                            static_cast<std::size_t>(logical - offset),
                            count * static_cast<std::size_t>(block));
                        if (geometry.version == 6)
                            v6_read_blocks(e.member, e.physical + logical - e.logical, target);
                        else v5_read_blocks(e.member, e.physical + logical - e.logical, target);
                    } else {
                        Bytes bytes(count * static_cast<std::size_t>(block));
                        if (geometry.version == 6)
                            v6_read_blocks(e.member, e.physical + logical - e.logical, bytes);
                        else v5_read_blocks(e.member, e.physical + logical - e.logical, bytes);
                        std::copy_n(bytes.data() + (copy_begin - logical),
                            static_cast<std::size_t>(copy_end - copy_begin),
                            destination.data() + (copy_begin - offset));
                    }
                    logical += count * block;
                }
            } else {
                disks[e.member]->read(e.physical + begin - e.logical,
                    destination.data() + static_cast<std::size_t>(begin - offset),
                    static_cast<std::size_t>(stop - begin));
            }
        }
    }
    std::vector<Extent> v4_plan(const File& file, std::uint64_t logical,
                                std::uint32_t length) const {
        struct Candidate {
            std::size_t member;
            std::uint64_t physical;
            std::uint64_t available;
            bool from_tail;
        };
        const auto block = static_cast<std::uint64_t>(geometry.io_block);
        std::vector<Extent> result;
        std::vector<std::uint64_t> planned_tails = data_tail;
        std::set<std::pair<std::size_t, std::uint64_t>> selected_free;
        std::uint64_t remaining = length;
        std::uint64_t position = logical;
        std::optional<std::size_t> preferred;
        if (!file.indexed.empty()) {
            const auto& last = file.indexed.rbegin()->second;
            if (last.logical + last.length == logical &&
                last.physical + allocation_bytes(last.length, geometry) == data_tail[last.member])
                preferred = last.member;
        }
        while (remaining) {
            std::optional<Candidate> chosen;
            if (preferred) {
                const auto member = *preferred;
                const auto tail = planned_tails[member];
                const auto available = data_end(member) - tail;
                if (available >= block) chosen = Candidate{member, tail, available, true};
            }
            if (!chosen) {
                // Best fit from reclaimed space, then virgin tail space.
                for (std::size_t member = 0; member < disks.size(); ++member) {
                    for (auto it = free_ranges[member].by_size.lower_bound({remaining, 0});
                         it != free_ranges[member].by_size.end(); ++it) {
                        if (!selected_free.contains({member, it->second})) {
                            if (!chosen || (!chosen->from_tail && it->first < chosen->available))
                                chosen = Candidate{member, it->second, it->first, false};
                            break;
                        }
                    }
                }
            }
            if (!chosen) {
                for (std::size_t member = 0; member < disks.size(); ++member) {
                    const auto tail = planned_tails[member];
                    const auto available = data_end(member) - tail;
                    if (available >= remaining &&
                        (!chosen || available > chosen->available))
                        chosen = Candidate{member, tail, available, true};
                }
            }
            if (!chosen) {
                for (std::size_t member = 0; member < disks.size(); ++member) {
                    const auto tail = planned_tails[member];
                    const auto available = data_end(member) - tail;
                    if (available >= block &&
                        (!chosen || available > chosen->available))
                        chosen = Candidate{member, tail, available, true};
                    for (auto it = free_ranges[member].by_size.rbegin();
                         it != free_ranges[member].by_size.rend(); ++it) {
                        if (!selected_free.contains({member, it->second})) {
                            if (!chosen || it->first > chosen->available)
                                chosen = Candidate{member, it->second, it->first, false};
                            break;
                        }
                    }
                }
            }
            if (!chosen) throw Error(ErrorCode::NoSpace, "pool is out of data space");
            const auto used = std::min<std::uint64_t>(remaining, chosen->available);
            if (used < block || used % block != 0)
                throw Error(ErrorCode::Corrupt, "unaligned v4 free range");
            result.push_back(Extent{position, chosen->physical,
                static_cast<std::uint32_t>(used), static_cast<std::uint8_t>(chosen->member)});
            // V6 also journals up to two group-root updates per small extent.
            // 64 extents keep even a 255-byte name below the 4 KiB slot.
            if (result.size() > (geometry.version == 6 ? 64U : 180U))
                throw Error(ErrorCode::JournalFull, "v4 write is too fragmented for one record");
            if (chosen->from_tail) planned_tails[chosen->member] += used;
            else selected_free.emplace(chosen->member, chosen->physical);
            position += used;
            remaining -= used;
            preferred.reset();
        }
        return result;
    }
    void ensure_v4_journal_room() {
        if (journal_tail + 2 * geometry.io_block <=
            v4_epoch_offset(active_epoch) + kV4EpochBytes) return;
        try {
            rotate_v4_checkpoint();
        } catch (const Error& error) {
            // A full snapshot is rejected before rotation writes anything.
            if (error.code() != ErrorCode::JournalFull) writable = false;
            throw;
        } catch (...) {
            writable = false;
            throw;
        }
    }
    void apply(const Bytes& payload) {
        Reader r{payload};
        const auto op = r.get8();
        if (op == 1) {
            const auto name = r.name(geometry.version == 6);
            const auto member = r.get8();
            const auto directory = geometry.version == 6 ? r.get8() : 0;
            const auto attributes = geometry.version == 6 ? r.get32() : 0;
            const auto created = geometry.version == 6 ? r.get64() : 0;
            r.done();
            if ((geometry.version >= 3 ? member != 255 : member > 1) || files.contains(name))
                throw Error(ErrorCode::Corrupt, "invalid create journal entry");
            if (geometry.version == 6) {
                require_parent_directory(name);
                if (directory > 1 || !created || next_file_id < 2 ||
                    next_file_id == std::numeric_limits<std::uint64_t>::max() ||
                    !valid_attributes(attributes, directory != 0))
                    throw Error(ErrorCode::Corrupt, "invalid v6 create metadata");
            }
            File file;
            file.entry = Entry{name, 0, member};
            if (geometry.version == 6) {
                file.entry.id = next_file_id++;
                file.entry.is_directory = directory != 0;
                file.entry.attributes = attributes;
                file.entry.creation_time = created;
                file.entry.last_access_time = created;
                file.entry.last_write_time = created;
                file.entry.change_time = created;
                id_paths.emplace(file.entry.id, name);
                touch_parent(name, created);
            }
            files.emplace(name, std::move(file));
        } else if (op == 2) {
            const auto name = r.name(geometry.version == 6);
            const auto logical = r.get64();
            auto& file = find(name);
            if (geometry.version == 6 && file.entry.is_directory)
                throw Error(ErrorCode::Corrupt, "write targets a directory");
            if (geometry.version >= 4) {
                const auto total_length = r.get32();
                const auto result_size = r.get64();
                const auto extent_count = r.get8();
                if (!total_length || total_length % geometry.io_block != 0 ||
                    !extent_count || logical % geometry.io_block != 0 ||
                    logical > std::numeric_limits<std::uint64_t>::max() - total_length ||
                    result_size < file.entry.size ||
                    result_size > std::max(file.entry.size, logical + total_length))
                    throw Error(ErrorCode::Corrupt, "invalid v4 write journal entry");
                std::vector<Extent> new_extents;
                new_extents.reserve(extent_count);
                std::set<std::pair<std::uint8_t, std::uint64_t>> affected_groups;
                std::uint64_t consumed = 0;
                for (std::size_t i = 0; i < extent_count; ++i) {
                    const auto member = r.get8();
                    const auto physical = r.get64();
                    const auto length = r.get32();
                    if (member >= disks.size() || !length ||
                        length % geometry.io_block != 0 ||
                        length > total_length - consumed ||
                        physical < geometry.data_offset ||
                        physical % geometry.io_block != 0 ||
                        physical > data_end(member) ||
                        length > data_end(member) - physical)
                        throw Error(ErrorCode::Corrupt, "invalid v4 write extent");
                    new_extents.push_back(Extent{logical + consumed, physical, length, member});
                    if (geometry.version == 6) {
                        const auto first = (physical - geometry.data_offset) /
                            (kV6GroupBlocks * geometry.io_block);
                        const auto last = (physical + length - 1 - geometry.data_offset) /
                            (kV6GroupBlocks * geometry.io_block);
                        for (auto group = first; group <= last; ++group)
                            affected_groups.emplace(member, group);
                    }
                    consumed += length;
                }
                struct RootUpdate {
                    std::uint8_t member;
                    std::uint64_t group;
                    V6Root root;
                };
                std::vector<RootUpdate> root_updates;
                if (geometry.version == 6) {
                    const auto count = r.get8();
                    if (count != affected_groups.size())
                        throw Error(ErrorCode::Corrupt, "v6 write checksum-group count differs");
                    std::set<std::pair<std::uint8_t, std::uint64_t>> seen;
                    for (std::size_t i = 0; i < count; ++i) {
                        const auto member = r.get8();
                        const auto group = r.get64();
                        const auto slot = r.get8();
                        const auto hash = r.get64();
                        const auto key = std::pair{member, group};
                        if (!affected_groups.contains(key) || !seen.insert(key).second ||
                            slot > 1)
                            throw Error(ErrorCode::Corrupt, "invalid v6 checksum root update");
                        const auto current = v6_roots[member].find(group);
                        if (slot != (current == v6_roots[member].end() ? 0 :
                                     1 - current->second.slot))
                            throw Error(ErrorCode::Corrupt, "v6 checksum root did not alternate");
                        root_updates.push_back(RootUpdate{member, group, V6Root{slot, hash}});
                    }
                }
                const auto changed = geometry.version == 6 ? r.get64() : 0;
                r.done();
                if (geometry.version == 6 && !changed)
                    throw Error(ErrorCode::Corrupt, "invalid v6 modification time");
                if (consumed != total_length)
                    throw Error(ErrorCode::Corrupt, "v4 write extent lengths disagree");
                for (const auto& e : new_extents) {
                    if (e.physical == data_tail[e.member]) {
                        data_tail[e.member] += e.length;
                    } else if (!free_ranges[e.member].consume(e.physical, e.length)) {
                        throw Error(ErrorCode::Corrupt, "v4 write reuses a live data block");
                    }
                }
                v4_splice(file, logical, logical + total_length, new_extents);
                file.entry.size = result_size;
                if (geometry.version == 6) {
                    file.entry.last_write_time = changed;
                    file.entry.change_time = changed;
                }
                for (const auto& update : root_updates)
                    v6_roots[update.member][update.group] = update.root;
            } else if (geometry.version == 3) {
                const auto total_length = r.get32();
                const auto extent_count = r.get8();
                if (!total_length || !extent_count || extent_count > disks.size() ||
                    logical > std::numeric_limits<std::uint64_t>::max() - total_length)
                    throw Error(ErrorCode::Corrupt, "invalid v3 write journal entry");
                auto planned_tails = data_tail;
                std::vector<Extent> new_extents;
                new_extents.reserve(extent_count);
                std::uint64_t consumed = 0;
                for (std::size_t i = 0; i < extent_count; ++i) {
                    const auto member = r.get8();
                    const auto physical = r.get64();
                    const auto length = r.get32();
                    if (member >= disks.size() || !length ||
                        length > total_length - consumed)
                        throw Error(ErrorCode::Corrupt, "invalid v3 write extent");
                    const auto occupied = allocation_bytes(length, geometry);
                    if (physical != planned_tails[member] ||
                        physical % geometry.io_block != 0 ||
                        occupied > disks[member]->header.bytes - physical)
                        throw Error(ErrorCode::Corrupt, "invalid v3 write extent location");
                    new_extents.push_back(Extent{logical + consumed, physical, length, member});
                    planned_tails[member] = physical + occupied;
                    consumed += length;
                }
                r.done();
                if (consumed != total_length)
                    throw Error(ErrorCode::Corrupt, "v3 write extent lengths disagree");
                file.extents.insert(file.extents.end(), new_extents.begin(), new_extents.end());
                file.entry.size = std::max(file.entry.size, logical + total_length);
                data_tail = std::move(planned_tails);
            } else {
                const auto physical = r.get64();
                const auto length = r.get32();
                r.done();
                const auto member = file.entry.member;
                const auto occupied = allocation_bytes(length, geometry);
                if (!length || logical > std::numeric_limits<std::uint64_t>::max() - length ||
                    physical < geometry.data_offset ||
                    occupied > disks[member]->header.bytes - geometry.data_offset ||
                    physical > disks[member]->header.bytes - occupied ||
                    (geometry.version == 2 && (physical % geometry.io_block != 0 ||
                        physical != data_tail[member])))
                    throw Error(ErrorCode::Corrupt, "invalid write journal entry");
                file.extents.push_back(Extent{logical, physical, length, member});
                file.entry.size = std::max(file.entry.size, logical + length);
                data_tail[member] = std::max(data_tail[member], physical + occupied);
            }
        } else if (op == 3) {
            const auto name = r.name(geometry.version == 6);
            const auto size = r.get64();
            const auto changed = geometry.version == 6 ? r.get64() : 0;
            r.done();
            auto& file = find(name);
            if (geometry.version == 6 && (file.entry.is_directory || !changed))
                throw Error(ErrorCode::Corrupt, "invalid v6 truncate metadata");
            if (geometry.version >= 4 && size < file.entry.size) {
                auto it = file.indexed.lower_bound(size);
                if (it != file.indexed.begin()) {
                    auto prev = std::prev(it);
                    if (prev->first + prev->second.length > size) {
                        auto& e = prev->second;
                        const auto old_length = e.length;
                        e.length = static_cast<std::uint32_t>(size - e.logical);
                        const auto kept = allocation_bytes(e.length, geometry);
                        const auto old_allocated = allocation_bytes(old_length, geometry);
                        if (kept < old_allocated)
                            v4_release_range(e.member, e.physical + kept,
                                             old_allocated - kept);
                    }
                }
                while (it != file.indexed.end()) {
                    v4_release(it->second);
                    it = file.indexed.erase(it);
                }
            }
            if (size < file.entry.size) {
                if (geometry.version < 4) {
                    auto& extents = file.extents;
                    extents.erase(std::remove_if(extents.begin(), extents.end(),
                        [size](const Extent& e) { return e.logical >= size; }), extents.end());
                    for (auto& e : extents)
                        if (e.length > size - e.logical) e.length = static_cast<std::uint32_t>(size - e.logical);
                }
            }
            file.entry.size = size;
            if (geometry.version == 6) {
                file.entry.last_write_time = changed;
                file.entry.change_time = changed;
            }
        } else if (op == 4) {
            const auto name = r.name(geometry.version == 6);
            const auto changed = geometry.version == 6 ? r.get64() : 0;
            r.done();
            auto it = files.find(name);
            if (it == files.end()) throw Error(ErrorCode::Corrupt, "invalid delete journal entry");
            if (geometry.version == 6 &&
                (!changed || (it->second.entry.is_directory && !directory_empty(name))))
                throw Error(ErrorCode::Corrupt, "invalid v6 delete operation");
            if (geometry.version >= 4)
                for (const auto& [logical, e] : it->second.indexed) v4_release(e);
            if (geometry.version == 6) {
                id_paths.erase(it->second.entry.id);
                touch_parent(name, changed);
            }
            files.erase(it);
        } else if (op == 5) {
            const auto old_name = r.name(geometry.version == 6);
            const auto new_name = r.name(geometry.version == 6);
            const auto changed = geometry.version == 6 ? r.get64() : 0;
            r.done();
            if (files.contains(new_name)) throw Error(ErrorCode::Corrupt, "invalid rename journal entry");
            if (geometry.version == 6) {
                require_parent_directory(new_name);
                const auto& old = find(old_name).entry;
                if (!changed || (old.is_directory && new_name.starts_with(old_name + '/')))
                    throw Error(ErrorCode::Corrupt, "invalid v6 directory rename");
                std::vector<std::pair<std::string, std::string>> moves;
                const auto prefix = old_name + '/';
                for (const auto& [path, file] : files) {
                    (void)file;
                    if (path != old_name && (!old.is_directory || !path.starts_with(prefix)))
                        continue;
                    auto target = new_name + path.substr(old_name.size());
                    validate_v6_path(target);
                    if (files.contains(target) &&
                        target != old_name && !target.starts_with(prefix))
                        throw Error(ErrorCode::Corrupt, "rename destination collides");
                    moves.emplace_back(path, std::move(target));
                }
                for (const auto& [source, target] : moves) {
                    auto node = files.extract(source);
                    node.key() = target;
                    node.mapped().entry.name = target;
                    node.mapped().entry.change_time = changed;
                    id_paths[node.mapped().entry.id] = target;
                    files.insert(std::move(node));
                }
                touch_parent(old_name, changed);
                touch_parent(new_name, changed);
            } else {
                auto node = files.extract(old_name);
                if (node.empty()) throw Error(ErrorCode::Corrupt, "invalid rename journal entry");
                node.key() = new_name;
                node.mapped().entry.name = new_name;
                files.insert(std::move(node));
            }
        } else if (op == 8 && geometry.version == 6) {
            const auto old_name = r.name(true);
            const auto new_name = r.name(true);
            const auto source_id = r.get64();
            const auto target_id = r.get64();
            const auto changed = r.get64();
            r.done();
            if (old_name == new_name || !changed || source_id < 2 || target_id < 2 ||
                source_id >= next_file_id || target_id >= next_file_id ||
                source_id == target_id)
                throw Error(ErrorCode::Corrupt, "invalid v6 replace operation");
            require_parent_directory(new_name);
            auto source = files.find(old_name);
            auto target = files.find(new_name);
            const auto source_path = id_paths.find(source_id);
            const auto target_path = id_paths.find(target_id);
            if (source == files.end() || target == files.end() ||
                source_path == id_paths.end() || target_path == id_paths.end() ||
                source_path->second != old_name || target_path->second != new_name ||
                source->second.entry.id != source_id ||
                target->second.entry.id != target_id ||
                source->second.entry.is_directory ||
                target->second.entry.is_directory ||
                (target->second.entry.attributes & kFileAttributeReadOnly))
                throw Error(ErrorCode::Corrupt, "invalid v6 replace target");
            for (const auto& [logical, extent] : target->second.indexed) {
                (void)logical;
                v4_release(extent);
            }
            id_paths.erase(target_id);
            files.erase(target);
            auto node = files.extract(source);
            node.key() = new_name;
            node.mapped().entry.name = new_name;
            node.mapped().entry.change_time = changed;
            id_paths[source_id] = new_name;
            files.insert(std::move(node));
            touch_parent(old_name, changed);
            touch_parent(new_name, changed);
        } else if (op == 6 && geometry.version == 6) {
            const auto id = r.get64();
            const auto attributes = r.get32();
            const auto creation = r.get64();
            const auto access = r.get64();
            const auto write = r.get64();
            const auto change = r.get64();
            r.done();
            auto& entry = find_by_id(id);
            if (attributes != kUnchangedAttributes) {
                if (!valid_attributes(attributes, entry.is_directory))
                    throw Error(ErrorCode::Corrupt, "v6 entry type cannot change");
                entry.attributes = attributes;
            }
            if (creation) entry.creation_time = creation;
            if (access) entry.last_access_time = access;
            if (write) entry.last_write_time = write;
            if (change) entry.change_time = change;
        } else if (op == 7 && geometry.version == 6) {
            const auto name = r.name(true);
            const auto attributes = r.get32();
            const auto changed = r.get64();
            r.done();
            auto& file = find(name);
            if (!changed || file.entry.is_directory ||
                (file.entry.attributes & kFileAttributeReadOnly) ||
                !valid_attributes(attributes, false))
                throw Error(ErrorCode::Corrupt, "invalid v6 overwrite operation");
            for (const auto& [logical, extent] : file.indexed) {
                (void)logical;
                v4_release(extent);
            }
            file.indexed.clear();
            file.entry.size = 0;
            file.entry.attributes = attributes;
            file.entry.last_write_time = changed;
            file.entry.change_time = changed;
        } else {
            throw Error(ErrorCode::Corrupt, "unsupported journal operation");
        }
    }
    void commit(const Bytes& payload) {
        if (!writable) throw Error(ErrorCode::ReadOnly,
            "pool is read-only after journal divergence or a failed write");
        const auto record = encode_record(next_sequence, payload, geometry);
        const auto end_marker_bytes = geometry.version >= 2 ? geometry.io_block : kRecordHeaderBytes;
        try {
            if (geometry.version >= 4) {
                if (journal_tail + record.size() + end_marker_bytes >
                    v4_epoch_offset(active_epoch) + kV4EpochBytes)
                    rotate_v4_checkpoint();
            } else if (journal_tail + record.size() + end_marker_bytes >
                       geometry.journal_offset + geometry.journal_bytes) {
                throw Error(ErrorCode::JournalFull, "metadata journal is full");
            }
            const Bytes end_marker(end_marker_bytes);
            for (auto& disk : disks) {
                disk->write(journal_tail, record.data(), record.size());
                disk->write(journal_tail + record.size(), end_marker.data(), end_marker.size());
                disk->sync();
            }
            apply(payload);
            journal_tail += record.size();
            ++next_sequence;
            if (geometry.version == 6 && !payload.empty() &&
                byte_at(payload, 0) == 2) {
                const char* force = std::getenv("POOLFS_TEST_V6_CHECKPOINT_AFTER_WRITE");
                if (force && std::strcmp(force, "1") == 0)
                    rotate_v4_checkpoint();
            }
        } catch (...) {
            writable = false;
            throw;
        }
    }
    void rotate_v4_checkpoint() {
        const auto payload = v4_snapshot();
        if (payload.size() > kV4CheckpointSlotBytes - geometry.io_block)
            throw Error(ErrorCode::JournalFull, "v4 checkpoint metadata area is full");
        const auto target_epoch = static_cast<std::uint8_t>(1 - active_epoch);
        const auto target_slot = static_cast<std::uint8_t>(checkpoint_generation % 2);
        const auto old_slot = static_cast<std::uint8_t>(1 - target_slot);
        const Bytes end_marker(geometry.io_block);
        for (auto& disk : disks) {
            disk->write(v4_epoch_offset(target_epoch), end_marker.data(), end_marker.size());
            disk->sync();
        }
        V4Checkpoint cp;
        cp.generation = checkpoint_generation + 1;
        cp.next_sequence = next_sequence;
        cp.epoch = target_epoch;
        cp.payload = payload;
        for (auto& disk : disks) write_v4_checkpoint(*disk, target_slot, cp, geometry);
        // Retire the old root only after every member has a durable new root.
        // A later copy-on-write allocation may reuse blocks the old root named.
        for (auto& disk : disks) {
            disk->write(v4_checkpoint_offset(old_slot), end_marker.data(), end_marker.size());
            disk->sync();
        }
        checkpoint_generation = cp.generation;
        active_epoch = target_epoch;
        journal_tail = v4_epoch_offset(active_epoch);
        if (geometry.version == 6) {
            capture_v6_checkpoint();
            rebuild_v4_free_ranges();
        }
    }
};

void format_images(const std::filesystem::path& first,
                   const std::filesystem::path& second,
                   std::uint64_t image_bytes, const Geometry& geometry) {
    const auto minimum = geometry.version == 1 ? Pool::kMinimumImageBytes : Pool::kMinimumRawBytes;
    if (image_bytes < minimum ||
        image_bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
        image_bytes % geometry.io_block != 0)
        throw Error(ErrorCode::InvalidArgument, "unsupported member image size");
    if (first == second || std::filesystem::exists(first) || std::filesystem::exists(second))
        throw Error(ErrorCode::InvalidArgument, "format requires two new, distinct image paths");
    const auto pool_id = random_id();
    auto id0 = random_id(), id1 = random_id();
    while (id0 == id1) id1 = random_id();
    auto a = std::make_unique<DiskImage>(first, true);
    auto b = std::make_unique<DiskImage>(second, true);
    a->resize(image_bytes);
    b->resize(image_bytes);
    DiskMember d0(std::move(a)), d1(std::move(b));
    if (geometry.version == 2) {
        const Bytes end_marker(geometry.io_block);
        d0.write(geometry.journal_offset, end_marker.data(), end_marker.size());
        d1.write(geometry.journal_offset, end_marker.data(), end_marker.size());
        d0.sync();
        d1.sync();
    }
    d0.write_header(Header{pool_id, id0, 0, image_bytes, 2, {}}, geometry);
    d1.write_header(Header{pool_id, id1, 1, image_bytes, 2, {}}, geometry);
}

std::vector<ImageMemberSpec> equal_size_members(
    const std::vector<std::filesystem::path>& paths, std::uint64_t bytes) {
    std::vector<ImageMemberSpec> members;
    members.reserve(paths.size());
    for (const auto& path : paths) members.push_back(ImageMemberSpec{path, bytes});
    return members;
}

void format_v3_images(const std::vector<ImageMemberSpec>& members) {
    if (members.size() < 2 || members.size() > kMaximumV3Members)
        throw Error(ErrorCode::InvalidArgument, "unsupported v3 image geometry");
    std::vector<std::wstring> normalized_paths;
    normalized_paths.reserve(members.size());
    std::uint64_t total_capacity = 0;
    for (const auto& member : members) {
        if (member.bytes < Pool::kMinimumRawBytes ||
            member.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            member.bytes % kRawV3.io_block != 0 ||
            member.bytes - kRawV3.data_offset >
                std::numeric_limits<std::uint64_t>::max() - total_capacity)
            throw Error(ErrorCode::InvalidArgument, "unsupported v3 image geometry");
        total_capacity += member.bytes - kRawV3.data_offset;
        if (member.path.empty() || std::filesystem::exists(member.path))
            throw Error(ErrorCode::InvalidArgument, "v3 format requires new image paths");
        auto normalized = std::filesystem::absolute(member.path).lexically_normal().wstring();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (std::find(normalized_paths.begin(), normalized_paths.end(), normalized) !=
            normalized_paths.end())
            throw Error(ErrorCode::InvalidArgument, "v3 image paths must be distinct");
        normalized_paths.push_back(std::move(normalized));
    }
    const auto pool_id = random_id();
    std::vector<ManifestEntry> manifest;
    manifest.reserve(members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto id = random_id();
        bool duplicate = true;
        while (duplicate) {
            duplicate = false;
            for (const auto& entry : manifest) duplicate |= entry.member_id == id;
            if (duplicate) id = random_id();
        }
        manifest.push_back(ManifestEntry{id, members[i].bytes});
    }
    std::vector<std::unique_ptr<DiskMember>> disks;
    disks.reserve(members.size());
    for (const auto& member : members) {
        auto image = std::make_unique<DiskImage>(member.path, true);
        image->resize(member.bytes);
        disks.push_back(std::make_unique<DiskMember>(std::move(image)));
    }
    const Bytes end_marker(kRawV3.io_block);
    for (auto& disk : disks) {
        disk->write(kRawV3.journal_offset, end_marker.data(), end_marker.size());
        disk->sync();
    }
    for (std::size_t i = 0; i < disks.size(); ++i)
        disks[i]->write_header(Header{pool_id, manifest[i].member_id,
            static_cast<std::uint8_t>(i), members[i].bytes,
            static_cast<std::uint8_t>(disks.size()), manifest}, kRawV3);
}

void format_v4_images(const std::vector<ImageMemberSpec>& members,
                      const Geometry& geometry,
                      std::string_view password = {}, bool encrypted = false) {
    if (encrypted && (geometry.version != 6 || password.empty()))
        throw Error(ErrorCode::InvalidArgument, "encrypted format requires a password");
    if (members.size() < (geometry.version == 6 ? 1 : 2) ||
        members.size() > kMaximumV3Members)
        throw Error(ErrorCode::InvalidArgument, "unsupported v4-v6 image geometry");
    std::vector<std::wstring> normalized_paths;
    normalized_paths.reserve(members.size());
    const auto minimum = geometry.version == 6 ? Pool::kMinimumV6ImageBytes :
        geometry.version == 5 ? Pool::kMinimumV5ImageBytes :
        Pool::kMinimumV4ImageBytes;
    std::uint64_t total_capacity = 0;
    for (const auto& member : members) {
        if (member.bytes < minimum ||
            member.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            member.bytes % geometry.io_block != 0)
            throw Error(ErrorCode::InvalidArgument, "unsupported v4-v6 image geometry");
        const auto data_end = geometry.version == 6 ? v6_data_end(member.bytes) :
            geometry.version == 5 ? v5_data_end(member.bytes) : member.bytes;
        const auto contribution = data_end - geometry.data_offset;
        if (contribution > std::numeric_limits<std::uint64_t>::max() - total_capacity)
            throw Error(ErrorCode::InvalidArgument, "v4-v6 image capacity exceeds range");
        total_capacity += contribution;
        if (member.path.empty() || std::filesystem::exists(member.path))
            throw Error(ErrorCode::InvalidArgument, "v4 format requires new image paths");
        auto normalized = std::filesystem::absolute(member.path).lexically_normal().wstring();
        std::transform(normalized.begin(), normalized.end(), normalized.begin(),
            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        if (std::find(normalized_paths.begin(), normalized_paths.end(), normalized) !=
            normalized_paths.end())
            throw Error(ErrorCode::InvalidArgument, "v4 image paths must be distinct");
        normalized_paths.push_back(std::move(normalized));
    }
    std::array<std::byte, kEncryptionSaltBytes> salt{};
    std::optional<crypto::KeyMaterial> keys;
    if (encrypted) {
        crypto::random_bytes(salt);
        keys.emplace(crypto::derive_keys(password, salt, kEncryptionIterations));
    }
    const auto pool_id = encrypted ? secure_random_id() : random_id();
    std::vector<ManifestEntry> manifest;
    manifest.reserve(members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto id = encrypted ? secure_random_id() : random_id();
        bool duplicate = true;
        while (duplicate) {
            duplicate = false;
            for (const auto& entry : manifest) duplicate |= entry.member_id == id;
            if (duplicate) id = encrypted ? secure_random_id() : random_id();
        }
        manifest.push_back(ManifestEntry{id, members[i].bytes});
    }
    std::vector<std::unique_ptr<DiskMember>> disks;
    disks.reserve(members.size());
    for (const auto& member : members) {
        auto image = std::make_unique<DiskImage>(member.path, true);
        image->resize(member.bytes);
        disks.push_back(std::make_unique<DiskMember>(std::move(image), true));
    }
    if (encrypted) {
        for (std::size_t i = 0; i < disks.size(); ++i) {
            disks[i]->header.encrypted = true;
            disks[i]->header.index = static_cast<std::uint8_t>(i);
            disks[i]->set_cipher(keys->xts_key());
        }
    }
    const Bytes end_marker(kRawV4.io_block);
    V4Checkpoint initial{1, 1, 0, empty_v4_snapshot(members.size(), geometry)};
    for (auto& disk : disks) {
        disk->write(v4_epoch_offset(0), end_marker.data(), end_marker.size());
        disk->write(v4_epoch_offset(1), end_marker.data(), end_marker.size());
        disk->sync();
        write_v4_checkpoint(*disk, 0, initial, geometry);
    }
    for (std::size_t i = 0; i < disks.size(); ++i) {
        Header header{pool_id, manifest[i].member_id,
            static_cast<std::uint8_t>(i), members[i].bytes,
            static_cast<std::uint8_t>(disks.size()), manifest};
        header.encrypted = encrypted;
        header.salt = salt;
        header.iterations = encrypted ? kEncryptionIterations : 0;
        disks[i]->write_header(header, geometry,
            encrypted ? keys->mac_key() : std::span<const std::byte>{});
    }
}

void Pool::Format(const std::filesystem::path& first,
                  const std::filesystem::path& second,
                  std::uint64_t image_bytes) {
    format_images(first, second, image_bytes, kImageV1);
}
void Pool::FormatV2Images(const std::filesystem::path& first,
                          const std::filesystem::path& second,
                          std::uint64_t image_bytes) {
    format_images(first, second, image_bytes, kRawV2);
}
void Pool::FormatV3Images(const std::vector<std::filesystem::path>& members,
                          std::uint64_t image_bytes) {
    FormatV3Images(equal_size_members(members, image_bytes));
}
void Pool::FormatV3Images(const std::vector<ImageMemberSpec>& members) {
    format_v3_images(members);
}
void Pool::FormatV4Images(const std::vector<std::filesystem::path>& members,
                          std::uint64_t image_bytes) {
    FormatV4Images(equal_size_members(members, image_bytes));
}
void Pool::FormatV4Images(const std::vector<ImageMemberSpec>& members) {
    format_v4_images(members, kRawV4);
}
void Pool::FormatV5Images(const std::vector<std::filesystem::path>& members,
                          std::uint64_t image_bytes) {
    FormatV5Images(equal_size_members(members, image_bytes));
}
void Pool::FormatV5Images(const std::vector<ImageMemberSpec>& members) {
    format_v4_images(members, kRawV5);
}
void Pool::FormatV6Images(const std::vector<std::filesystem::path>& members,
                          std::uint64_t image_bytes) {
    FormatV6Images(equal_size_members(members, image_bytes));
}
void Pool::FormatV6Images(const std::vector<ImageMemberSpec>& members) {
    format_v4_images(members, kRawV6);
}
void Pool::FormatV6EncryptedImages(const std::vector<std::filesystem::path>& members,
                                    std::uint64_t image_bytes,
                                    std::string_view password) {
    FormatV6EncryptedImages(equal_size_members(members, image_bytes), password);
}
void Pool::FormatV6EncryptedImages(const std::vector<ImageMemberSpec>& members,
                                    std::string_view password) {
    format_v4_images(members, kRawV6, password, true);
}
void Pool::GrowV6Images(const std::vector<ImageGrowSpec>& members,
                        std::string_view password) {
    if (members.empty() || members.size() > kMaximumV3Members)
        throw Error(ErrorCode::InvalidArgument, "grow needs every v6 member");
    std::vector<std::filesystem::path> originals;
    std::vector<std::filesystem::path> replacements;
    std::vector<std::wstring> distinct;
    bool growing = false;
    for (const auto& spec : members) {
        if (spec.bytes < kMinimumV6ImageBytes ||
            spec.bytes > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
            spec.bytes % kRawV6.io_block != 0)
            throw Error(ErrorCode::InvalidArgument, "invalid expanded member size");
        for (const auto& path : {spec.source, spec.destination}) {
            if (path.empty()) throw Error(ErrorCode::InvalidArgument, "empty grow path");
            auto normalized = std::filesystem::absolute(path).lexically_normal().wstring();
            std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            if (std::find(distinct.begin(), distinct.end(), normalized) != distinct.end())
                throw Error(ErrorCode::InvalidArgument, "grow paths must be distinct");
            distinct.push_back(std::move(normalized));
        }
        if (std::filesystem::exists(spec.destination))
            throw Error(ErrorCode::AlreadyExists, "expanded destination already exists");
        originals.push_back(spec.source);
        replacements.push_back(spec.destination);
    }
    // A read-only open validates the common journal/checkpoint before any copy.
    {
        const bool encrypted = V6Encrypted(originals.front());
        if (encrypted) {
            Pool pool(V6ReadOnlyImageTag{}, originals, password);
            if (pool.RecoveredIncompleteTail() || pool.Scrub({}) != 0)
                throw Error(ErrorCode::Corrupt, "source pool needs repair or has damaged data");
        } else {
            Pool pool(V6ReadOnlyImageTag{}, originals);
            if (pool.RecoveredIncompleteTail() || pool.Scrub({}) != 0)
                throw Error(ErrorCode::Corrupt, "source pool needs repair or has damaged data");
        }
    }
    std::vector<std::unique_ptr<DiskMember>> sources;
    std::vector<DiskImage*> raw_sources;
    std::vector<const ImageGrowSpec*> by_index(members.size(), nullptr);
    sources.reserve(members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto raw = std::make_unique<DiskImage>(members[i].source, false, true);
        auto* raw_ptr = raw.get();
        auto disk = std::make_unique<DiskMember>(std::move(raw));
        disk->load_header(kRawV6);
        if (disk->header.index >= members.size() || by_index[disk->header.index])
            throw Error(ErrorCode::Corrupt, "grow member list is incomplete or duplicated");
        if (members[i].bytes < disk->header.bytes)
            throw Error(ErrorCode::InvalidArgument, "grow cannot shrink a member");
        growing |= members[i].bytes > disk->header.bytes;
        by_index[disk->header.index] = &members[i];
        raw_sources.push_back(raw_ptr);
        sources.push_back(std::move(disk));
    }
    if (!growing) throw Error(ErrorCode::InvalidArgument, "no member size increased");
    const auto reference = sources.front()->header;
    for (const auto& source : sources)
        if (source->header.pool_id != reference.pool_id ||
            source->header.manifest != reference.manifest ||
            source->header.encrypted != reference.encrypted)
            throw Error(ErrorCode::Corrupt, "grow members disagree on pool manifest");
    std::optional<crypto::KeyMaterial> keys;
    if (reference.encrypted) {
        if (password.empty()) throw Error(ErrorCode::AccessDenied, "grow requires pool password");
        keys.emplace(crypto::derive_keys(password, reference.salt, reference.iterations));
        for (auto& source : sources) {
            source->verify_header_mac(keys->mac_key());
            source->set_cipher(keys->xts_key());
        }
    }
    auto manifest = reference.manifest;
    for (std::size_t i = 0; i < manifest.size(); ++i)
        manifest[i].bytes = by_index[i]->bytes;
    std::vector<std::unique_ptr<DiskMember>> destinations;
    destinations.reserve(members.size());
    Bytes block(1024 * 1024);
    Bytes page(kRawV6.io_block);
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto raw = std::make_unique<DiskImage>(members[i].destination, true);
        raw->resize(members[i].bytes);
        auto* target = raw.get();
        const auto old_bytes = sources[i]->header.bytes;
        for (std::uint64_t offset = 0; offset < old_bytes;) {
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(
                block.size(), old_bytes - offset));
            raw_sources[i]->read(offset, block.data(), length);
            if (std::any_of(block.begin(), block.begin() + length,
                           [](std::byte value) { return value != std::byte{0}; }))
                target->write(offset, block.data(), length);
            offset += length;
        }
        if (members[i].bytes != old_bytes) {
            // The old checksum tail becomes data space after growth. Do not
            // leave its bytes available to a later partial-block allocation.
            std::fill(block.begin(), block.end(), std::byte{0});
            for (auto offset = v6_checksum_start(old_bytes); offset < old_bytes;) {
                const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(
                    block.size(), old_bytes - offset));
                target->write(offset, block.data(), length);
                offset += length;
            }
        }
        auto disk = std::make_unique<DiskMember>(std::move(raw));
        disk->header = sources[i]->header;
        if (reference.encrypted) disk->set_cipher(keys->xts_key());
        if (members[i].bytes != old_bytes) {
            const auto groups = v6_groups(v6_data_blocks(old_bytes));
            for (std::uint64_t group = 0; group < groups; ++group)
                for (std::uint8_t slot = 0; slot < 2; ++slot) {
                    sources[i]->read(v6_checksum_page(old_bytes, group, slot),
                                     page.data(), page.size());
                    disk->write(v6_checksum_page(members[i].bytes, group, slot),
                                page.data(), page.size());
                }
        }
        disk->sync();
        destinations.push_back(std::move(disk));
    }
    for (std::size_t i = 0; i < destinations.size(); ++i) {
        auto header = sources[i]->header;
        header.bytes = members[i].bytes;
        header.manifest = manifest;
        destinations[i]->write_header(header, kRawV6,
            reference.encrypted ? keys->mac_key() : std::span<const std::byte>{});
    }
    destinations.clear();
    sources.clear();
    if (reference.encrypted) {
        Pool verified(V6ReadOnlyImageTag{}, replacements, password);
        if (verified.RecoveredIncompleteTail() || verified.Scrub({}) != 0)
            throw Error(ErrorCode::Corrupt, "expanded copy failed verification; originals are intact");
    } else {
        Pool verified(V6ReadOnlyImageTag{}, replacements);
        if (verified.RecoveredIncompleteTail() || verified.Scrub({}) != 0)
            throw Error(ErrorCode::Corrupt, "expanded copy failed verification; originals are intact");
    }
}
bool Pool::V6Encrypted(const std::filesystem::path& member) {
    DiskMember disk(std::make_unique<DiskImage>(member, false, true));
    disk.load_header(kRawV6);
    return disk.header.encrypted;
}
void Pool::FormatRaw(const RawMemberSpec& first, const RawMemberSpec& second) {
    auto a = std::make_unique<RawVolume>(first.volume_path, first.expected_disk_guid);
    auto b = std::make_unique<RawVolume>(second.volume_path, second.expected_disk_guid);
    if (a->info().disk_number == b->info().disk_number)
        throw Error(ErrorCode::InvalidArgument, "raw members must be on different disks");
    const auto check = [](RawVolume& volume) {
        if (volume.size() < kMinimumRawBytes || volume.size() % kRawV2.io_block != 0)
            throw Error(ErrorCode::InvalidArgument, "raw member size is unsupported");
        Bytes first_blocks(2 * kHeaderBytes);
        volume.read(0, first_blocks.data(), first_blocks.size());
        if (!std::all_of(first_blocks.begin(), first_blocks.end(),
            [](std::byte value) { return value == std::byte{0}; }))
            throw Error(ErrorCode::InvalidArgument,
                "raw member starts with nonzero data; refusing to overwrite it");
    };
    check(*a);
    check(*b);
    const auto pool_id = random_id();
    auto id0 = random_id(), id1 = random_id();
    while (id0 == id1) id1 = random_id();
    DiskMember d0(std::move(a)), d1(std::move(b));
    const Bytes end_marker(kRawV2.io_block);
    d0.write(kRawV2.journal_offset, end_marker.data(), end_marker.size());
    d1.write(kRawV2.journal_offset, end_marker.data(), end_marker.size());
    d0.sync();
    d1.sync();
    d0.write_header(Header{pool_id, id0, 0, d0.size(), 2, {}}, kRawV2);
    d1.write_header(Header{pool_id, id1, 1, d1.size(), 2, {}}, kRawV2);
}

void Pool::FormatRawV3(const std::vector<RawMemberSpec>& members) {
    auto volumes = open_raw_v3(members);
    constexpr std::size_t kScanChunkBytes = 1024 * 1024;
    Bytes scan_chunk(kScanChunkBytes);
    for (auto& volume : volumes) {
        for (std::uint64_t offset = 0; offset < kRawV3.data_offset;
             offset += kScanChunkBytes) {
            const auto length = static_cast<std::size_t>(std::min<std::uint64_t>(
                kScanChunkBytes, kRawV3.data_offset - offset));
            volume->read(offset, scan_chunk.data(), length);
            if (!std::all_of(scan_chunk.begin(), scan_chunk.begin() + length,
                [](std::byte value) { return value == std::byte{0}; }))
                throw Error(ErrorCode::InvalidArgument,
                    "raw member metadata region contains data; refusing to overwrite it");
        }
    }
    const auto pool_id = random_id();
    std::vector<ManifestEntry> manifest;
    manifest.reserve(volumes.size());
    for (const auto& volume : volumes) {
        auto id = random_id();
        bool duplicate = true;
        while (duplicate) {
            duplicate = false;
            for (const auto& entry : manifest) duplicate |= entry.member_id == id;
            if (duplicate) id = random_id();
        }
        manifest.push_back(ManifestEntry{id, volume->size()});
    }
    std::vector<std::unique_ptr<DiskMember>> disks;
    disks.reserve(volumes.size());
    for (auto& volume : volumes)
        disks.push_back(std::make_unique<DiskMember>(std::move(volume)));
    const Bytes end_marker(kRawV3.io_block);
    for (auto& disk : disks) {
        disk->write(kRawV3.journal_offset, end_marker.data(), end_marker.size());
        disk->sync();
    }
    for (std::size_t i = 0; i < disks.size(); ++i)
        disks[i]->write_header(Header{pool_id, manifest[i].member_id,
            static_cast<std::uint8_t>(i), manifest[i].bytes,
            static_cast<std::uint8_t>(disks.size()), manifest}, kRawV3);
}

Pool::Pool(const std::filesystem::path& first, const std::filesystem::path& second)
    : impl_(std::make_unique<Impl>(first, second, kImageV1)) {}
Pool::Pool(V2ImageTag, const std::filesystem::path& first,
           const std::filesystem::path& second)
    : impl_(std::make_unique<Impl>(first, second, kRawV2)) {}
Pool::Pool(const RawMemberSpec& first, const RawMemberSpec& second)
    : impl_(std::make_unique<Impl>(first, second)) {}
Pool::Pool(V3ImageTag, const std::vector<std::filesystem::path>& members)
    : impl_(std::make_unique<Impl>(V3ImageTag{}, members)) {}
Pool::Pool(V4ImageTag, const std::vector<std::filesystem::path>& members)
    : impl_(std::make_unique<Impl>(V4ImageTag{}, members)) {}
Pool::Pool(V5ImageTag, const std::vector<std::filesystem::path>& members)
    : impl_(std::make_unique<Impl>(V5ImageTag{}, members, false)) {}
Pool::Pool(V5ReadOnlyImageTag, const std::vector<std::filesystem::path>& members)
    : impl_(std::make_unique<Impl>(V5ImageTag{}, members, true)) {}
Pool::Pool(V6ImageTag, const std::vector<std::filesystem::path>& members)
    : impl_(std::make_unique<Impl>(V6ImageTag{}, members, false)) {}
Pool::Pool(V6ReadOnlyImageTag, const std::vector<std::filesystem::path>& members)
    : impl_(std::make_unique<Impl>(V6ImageTag{}, members, true)) {}
Pool::Pool(V6ImageTag, const std::vector<std::filesystem::path>& members,
           std::string_view password)
    : impl_(std::make_unique<Impl>(V6ImageTag{}, members, false, password, true)) {}
Pool::Pool(V6ReadOnlyImageTag, const std::vector<std::filesystem::path>& members,
           std::string_view password)
    : impl_(std::make_unique<Impl>(V6ImageTag{}, members, true, password, true)) {}
Pool::Pool(RawV3Tag, const std::vector<RawMemberSpec>& members)
    : impl_(std::make_unique<Impl>(RawV3Tag{}, members)) {}
Pool::~Pool() = default;

std::vector<Entry> Pool::List() const {
    std::lock_guard lock(impl_->mutex);
    std::vector<Entry> result;
    result.reserve(impl_->files.size());
    for (const auto& [name, file] : impl_->files) result.push_back(file.entry);
    return result;
}
std::vector<Entry> Pool::ListDirectory(const std::string& directory) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "directories require v6");
    if (!directory.empty() && !impl_->find(directory).entry.is_directory)
        throw Error(ErrorCode::InvalidArgument, "path is not a directory");
    std::vector<Entry> result;
    for (const auto& [name, file] : impl_->files)
        if (parent_path(name) == directory) {
            Entry child = file.entry;
            child.name = leaf_name(name);
            result.push_back(std::move(child));
        }
    return result;
}
Entry Pool::Root() const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "root metadata requires v6");
    return impl_->root_entry;
}
Entry Pool::Stat(const std::string& name) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->find(name).entry;
}
std::string Pool::NameForId(std::uint64_t id) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "file IDs require v6");
    if (id == 1) return {};
    const auto it = impl_->id_paths.find(id);
    if (it == impl_->id_paths.end()) throw Error(ErrorCode::NotFound, "file ID not found");
    return it->second;
}
void Pool::Create(const std::string& name, std::uint32_t attributes) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version == 6) {
        validate_v6_path(name);
        impl_->require_parent_directory(name);
    } else validate_name(name);
    if (impl_->files.contains(name)) throw Error(ErrorCode::AlreadyExists, "file already exists");
    if (impl_->geometry.version == 6 &&
        impl_->next_file_id == std::numeric_limits<std::uint64_t>::max())
        throw Error(ErrorCode::JournalFull, "v6 file ID space is exhausted");
    Bytes p;
    put8(p, 1);
    put_name(p, name);
    if (impl_->geometry.version >= 3) {
        put8(p, 255);
    } else {
        const auto free0 = impl_->disks[0]->header.bytes - impl_->data_tail[0];
        const auto free1 = impl_->disks[1]->header.bytes - impl_->data_tail[1];
        put8(p, free0 >= free1 ? 0 : 1);
    }
    if (impl_->geometry.version == 6) {
        attributes = normalized_attributes(attributes, false);
        put8(p, 0);
        put32(p, attributes);
        put64(p, current_filetime());
    }
    impl_->commit(p);
}
void Pool::MakeDirectory(const std::string& name, std::uint32_t attributes) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "directories require v6");
    validate_v6_path(name);
    impl_->require_parent_directory(name);
    if (impl_->files.contains(name)) throw Error(ErrorCode::AlreadyExists, "path already exists");
    if (impl_->next_file_id == std::numeric_limits<std::uint64_t>::max())
        throw Error(ErrorCode::JournalFull, "v6 file ID space is exhausted");
    attributes = normalized_attributes(attributes, true);
    Bytes p;
    put8(p, 1);
    put_name(p, name);
    put8(p, 255);
    put8(p, 1);
    put32(p, attributes);
    put64(p, current_filetime());
    impl_->commit(p);
}
std::size_t Pool::Read(const std::string& name, std::uint64_t offset,
                       std::span<std::byte> destination) {
    std::lock_guard lock(impl_->mutex);
    const auto& file = impl_->find(name);
    if (file.entry.is_directory)
        throw Error(ErrorCode::InvalidArgument, "cannot read a directory");
    if (offset >= file.entry.size) return 0;
    const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(destination.size(), file.entry.size - offset));
    if (count == 0) return 0;
    if (impl_->geometry.version >= 4) {
        try {
            impl_->v4_read_live(file, offset, destination.first(count));
        } catch (const Error& error) {
            if (error.code() != ErrorCode::Corrupt) throw;
            throw Error(ErrorCode::Corrupt, "file " + name + ": " + error.what());
        }
        return count;
    }
    std::fill_n(destination.data(), count, std::byte{0});
    const auto end = offset + count;
    struct Range { std::uint64_t begin, end; };
    std::vector<Range> uncovered{{offset, end}};
    std::vector<Range> next;
    // Later extents win. Visit them first so obsolete data is never read.
    for (auto it = file.extents.rbegin(); it != file.extents.rend() && !uncovered.empty(); ++it) {
        const auto& e = *it;
        const auto extent_end = e.logical + e.length;
        if (extent_end <= offset || e.logical >= end) continue;
        next.clear();
        for (const auto& gap : uncovered) {
            const auto begin = std::max(gap.begin, e.logical);
            const auto stop = std::min(gap.end, extent_end);
            if (begin >= stop) {
                next.push_back(gap);
                continue;
            }
            if (gap.begin < begin) next.push_back({gap.begin, begin});
            impl_->disks[e.member]->read(e.physical + (begin - e.logical),
                destination.data() + (begin - offset), static_cast<std::size_t>(stop - begin));
            if (stop < gap.end) next.push_back({stop, gap.end});
        }
        uncovered.swap(next);
    }
    return count;
}
std::size_t Pool::Write(const std::string& name, std::uint64_t offset,
                        std::span<const std::byte> source) {
    if (source.size() > std::numeric_limits<std::uint32_t>::max() ||
        offset > std::numeric_limits<std::uint64_t>::max() - source.size())
        throw Error(ErrorCode::InvalidArgument, "write is too large");
    std::lock_guard lock(impl_->mutex);
    const auto& file = impl_->find(name);
    if (file.entry.is_directory)
        throw Error(ErrorCode::InvalidArgument, "cannot write a directory");
    if (file.entry.attributes & kFileAttributeReadOnly)
        throw Error(ErrorCode::AccessDenied, "file is read-only");
    if (source.empty()) return 0;
    if (impl_->geometry.version >= 4) {
        if (!impl_->writable) throw Error(ErrorCode::ReadOnly, "pool is read-only after a failed write");
        const auto block = static_cast<std::uint64_t>(impl_->geometry.io_block);
        std::size_t written = 0;
        try {
        while (written < source.size()) {
            const auto chunk_offset = offset + written;
            auto chunk_length = std::min<std::size_t>(source.size() - written, 4 * 1024 * 1024);
            std::vector<Impl::Extent> planned;
            Bytes aligned;
            std::uint64_t logical = 0;
            bool retried_after_checkpoint = false;
            for (;;) {
                if (chunk_offset + chunk_length >
                    std::numeric_limits<std::uint64_t>::max() - (block - 1))
                    throw Error(ErrorCode::InvalidArgument, "write offset is too large");
                logical = chunk_offset / block * block;
                const auto end = allocation_bytes(chunk_offset + chunk_length, impl_->geometry);
                const auto aligned_length = static_cast<std::size_t>(end - logical);
                aligned.resize(aligned_length);
                impl_->v4_read_live(file, logical, aligned);
                std::copy_n(source.data() + written, chunk_length,
                    aligned.data() + static_cast<std::size_t>(chunk_offset - logical));
                try {
                    planned = impl_->v4_plan(file, logical,
                        static_cast<std::uint32_t>(aligned_length));
                    break;
                } catch (const Error& error) {
                    if (impl_->geometry.version == 6 &&
                        error.code() == ErrorCode::NoSpace &&
                        !retried_after_checkpoint) {
                        // The previous checkpoint can retain blocks that are
                        // no longer live. Refresh it once before reporting
                        // the pool full or reducing this write chunk.
                        try {
                            impl_->rotate_v4_checkpoint();
                        } catch (const Error& rotate_error) {
                            if (rotate_error.code() != ErrorCode::JournalFull)
                                impl_->writable = false;
                            throw;
                        } catch (...) {
                            impl_->writable = false;
                            throw;
                        }
                        retried_after_checkpoint = true;
                        continue;
                    }
                    if (chunk_length <= 1 ||
                        (error.code() != ErrorCode::JournalFull &&
                         error.code() != ErrorCode::NoSpace)) throw;
                    chunk_length = error.code() == ErrorCode::JournalFull
                        ? std::min<std::size_t>(chunk_length / 2, 256 * 1024)
                        : chunk_length / 2;
                    chunk_length = std::max<std::size_t>(chunk_length, 1);
                }
            }
            std::vector<Impl::V6PageWrite> page_writes;
            if (impl_->geometry.version == 6) {
                page_writes = impl_->v6_plan_pages(planned, aligned, logical);
                impl_->protect_v6_checkpoint_pages(page_writes);
            }
            Bytes payload;
            put8(payload, 2);
            put_name(payload, name);
            put64(payload, logical);
            put32(payload, static_cast<std::uint32_t>(aligned.size()));
            put64(payload, std::max(file.entry.size, chunk_offset + chunk_length));
            put8(payload, static_cast<std::uint8_t>(planned.size()));
            for (const auto& e : planned) {
                put8(payload, e.member);
                put64(payload, e.physical);
                put32(payload, e.length);
            }
            if (impl_->geometry.version == 6) {
                put8(payload, static_cast<std::uint8_t>(page_writes.size()));
                for (const auto& page : page_writes) {
                    put8(payload, page.member);
                    put64(payload, page.group);
                    put8(payload, page.slot);
                    put64(payload, page.hash);
                }
                put64(payload, current_filetime());
            }
            encode_record(impl_->next_sequence, payload, impl_->geometry);
            impl_->ensure_v4_journal_room();
            try {
                std::vector<bool> touched(impl_->disks.size());
                for (const auto& e : planned) {
                    const auto begin = static_cast<std::size_t>(e.logical - logical);
                    impl_->disks[e.member]->write(e.physical,
                        aligned.data() + begin, e.length);
                    if (impl_->geometry.version == 5) {
                        Bytes checksums;
                        checksums.reserve(static_cast<std::size_t>(e.length / block) * 8);
                        for (std::size_t at = 0; at < e.length; at += block)
                            put64(checksums, crc64(aligned.data() + begin + at,
                                                   static_cast<std::size_t>(block)));
                        impl_->disks[e.member]->write(
                            impl_->checksum_offset(e.member, e.physical),
                            checksums.data(), checksums.size());
                    }
                    touched[e.member] = true;
                }
                if (impl_->geometry.version == 6) {
                    static std::atomic<bool> torn_once{false};
                    const char* tear = std::getenv("POOLFS_TEST_V6_TEAR_CHECKSUM_PAGE");
                    for (const auto& page : page_writes) {
                        const auto position = v6_checksum_page(
                            impl_->disks[page.member]->header.bytes, page.group, page.slot);
                        if (tear && std::strcmp(tear, "1") == 0 &&
                            !torn_once.exchange(true)) {
                            impl_->disks[page.member]->write(position,
                                page.page.data(), page.page.size() / 2);
                            impl_->disks[page.member]->sync();
                            std::_Exit(87);
                        }
                        impl_->disks[page.member]->write(position,
                            page.page.data(), page.page.size());
                        touched[page.member] = true;
                    }
                }
                for (std::size_t i = 0; i < touched.size(); ++i)
                    if (touched[i]) impl_->disks[i]->sync();
            } catch (...) {
                impl_->writable = false;
                throw;
            }
            impl_->commit(payload);
            written += chunk_length;
        }
        } catch (const Error& error) {
            if (written && impl_->writable &&
                (error.code() == ErrorCode::NoSpace || error.code() == ErrorCode::JournalFull))
                return written;
            throw;
        }
        return written;
    }
    if (impl_->geometry.version == 3) {
        if (!impl_->writable) throw Error(ErrorCode::ReadOnly,
            "pool is read-only after journal divergence or a failed write");
        auto planned_tails = impl_->data_tail;
        std::vector<Impl::Extent> planned;
        std::uint64_t remaining = source.size();
        std::uint64_t logical = offset;
        const auto block = static_cast<std::uint64_t>(impl_->geometry.io_block);
        while (remaining) {
            std::size_t best = impl_->disks.size();
            std::uint64_t most_available = 0;
            for (std::size_t i = 0; i < impl_->disks.size(); ++i) {
                const auto free_bytes = impl_->disks[i]->header.bytes - planned_tails[i];
                const auto usable = (free_bytes / block) * block;
                if (usable > most_available) {
                    most_available = usable;
                    best = i;
                }
            }
            if (best == impl_->disks.size())
                throw Error(ErrorCode::NoSpace, "pool is out of data space");
            const auto length = static_cast<std::uint32_t>(std::min(remaining, most_available));
            planned.push_back(Impl::Extent{logical, planned_tails[best], length,
                                           static_cast<std::uint8_t>(best)});
            planned_tails[best] += allocation_bytes(length, impl_->geometry);
            logical += length;
            remaining -= length;
        }
        Bytes p;
        put8(p, 2);
        put_name(p, name);
        put64(p, offset);
        put32(p, static_cast<std::uint32_t>(source.size()));
        put8(p, static_cast<std::uint8_t>(planned.size()));
        for (const auto& e : planned) {
            put8(p, e.member);
            put64(p, e.physical);
            put32(p, e.length);
        }
        const auto encoded = encode_record(impl_->next_sequence, p, impl_->geometry);
        if (impl_->journal_tail + encoded.size() + impl_->geometry.io_block >
            impl_->geometry.journal_offset + impl_->geometry.journal_bytes)
            throw Error(ErrorCode::JournalFull, "metadata journal is full");
        try {
            std::vector<bool> touched(impl_->disks.size());
            for (const auto& e : planned) {
                const auto source_offset = static_cast<std::size_t>(e.logical - offset);
                const auto aligned_length = static_cast<std::size_t>(e.length / block * block);
                if (aligned_length)
                    impl_->disks[e.member]->write(e.physical,
                        source.data() + source_offset, aligned_length);
                const auto tail_length = static_cast<std::size_t>(e.length) - aligned_length;
                if (tail_length) {
                    Bytes padded(static_cast<std::size_t>(block), std::byte{0});
                    std::copy_n(source.data() + source_offset + aligned_length,
                                tail_length, padded.data());
                    impl_->disks[e.member]->write(e.physical + aligned_length,
                                                   padded.data(), padded.size());
                }
                touched[e.member] = true;
            }
            for (std::size_t i = 0; i < touched.size(); ++i)
                if (touched[i]) impl_->disks[i]->sync();
        } catch (...) {
            impl_->writable = false;
            throw;
        }
        impl_->commit(p);
        return source.size();
    }
    const auto member = file.entry.member;
    const auto physical = impl_->data_tail[member];
    const auto occupied = allocation_bytes(source.size(), impl_->geometry);
    if (occupied > impl_->disks[member]->header.bytes - physical)
        throw Error(ErrorCode::NoSpace, "member is out of data space");
    if (!impl_->writable) throw Error(ErrorCode::ReadOnly,
        "pool is read-only after journal divergence or a failed write");
    try {
        if (impl_->geometry.version == 2) {
            Bytes padded(static_cast<std::size_t>(occupied), std::byte{0});
            std::copy(source.begin(), source.end(), padded.begin());
            impl_->disks[member]->write(physical, padded.data(), padded.size());
        } else {
            impl_->disks[member]->write(physical, source.data(), source.size());
        }
        impl_->disks[member]->sync();
    } catch (...) {
        impl_->writable = false;
        throw;
    }
    Bytes p;
    put8(p, 2);
    put_name(p, name);
    put64(p, offset);
    put64(p, physical);
    put32(p, static_cast<std::uint32_t>(source.size()));
    impl_->commit(p);
    return source.size();
}
void Pool::Truncate(const std::string& name, std::uint64_t size) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->find(name).entry.is_directory)
        throw Error(ErrorCode::InvalidArgument, "cannot resize a directory");
    if (impl_->find(name).entry.attributes & kFileAttributeReadOnly)
        throw Error(ErrorCode::AccessDenied, "file is read-only");
    Bytes p;
    put8(p, 3);
    put_name(p, name);
    put64(p, size);
    if (impl_->geometry.version == 6) put64(p, current_filetime());
    impl_->commit(p);
}
void Pool::Overwrite(const std::string& name, std::uint32_t attributes,
                     bool replace_attributes) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "atomic overwrite requires v6");
    const auto& entry = impl_->find(name).entry;
    if (entry.is_directory)
        throw Error(ErrorCode::InvalidArgument, "cannot overwrite a directory");
    if (entry.attributes & kFileAttributeReadOnly)
        throw Error(ErrorCode::AccessDenied, "file is read-only");
    const auto requested = replace_attributes ? attributes : entry.attributes | attributes;
    const auto final_attributes = normalized_attributes(requested, false);
    Bytes p;
    put8(p, 7);
    put_name(p, name);
    put32(p, final_attributes);
    put64(p, current_filetime());
    impl_->commit(p);
}
void Pool::Remove(const std::string& name) {
    std::lock_guard lock(impl_->mutex);
    const auto& entry = impl_->find(name).entry;
    if (!entry.is_directory && (entry.attributes & kFileAttributeReadOnly))
        throw Error(ErrorCode::AccessDenied, "file is read-only");
    if (impl_->geometry.version == 6 && entry.is_directory &&
        !impl_->directory_empty(name))
        throw Error(ErrorCode::InvalidArgument, "directory is not empty");
    Bytes p;
    put8(p, 4);
    put_name(p, name);
    if (impl_->geometry.version == 6) put64(p, current_filetime());
    impl_->commit(p);
}
bool Pool::IsDirectoryEmpty(const std::string& name) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "directories require v6");
    if (!name.empty() && !impl_->find(name).entry.is_directory)
        throw Error(ErrorCode::InvalidArgument, "path is not a directory");
    return impl_->directory_empty(name);
}
void Pool::Rename(const std::string& old_name, const std::string& new_name,
                  bool replace_existing) {
    std::lock_guard lock(impl_->mutex);
    const auto& old = impl_->find(old_name).entry;
    if (old_name == new_name) return;
    if (impl_->geometry.version == 6) {
        validate_v6_path(new_name);
        impl_->require_parent_directory(new_name);
        if (old.is_directory && new_name.starts_with(old_name + '/'))
            throw Error(ErrorCode::InvalidArgument, "cannot move directory into itself");
        const auto prefix = old_name + '/';
        for (const auto& [path, file] : impl_->files) {
            (void)file;
            if (path.starts_with(prefix))
                validate_v6_path(new_name + path.substr(old_name.size()));
        }
    } else {
        if (replace_existing)
            throw Error(ErrorCode::InvalidArgument, "replace requires v6");
        validate_name(new_name);
    }
    const auto target = impl_->files.find(new_name);
    if (target != impl_->files.end()) {
        if (!replace_existing)
            throw Error(ErrorCode::AlreadyExists, "destination already exists");
        if (old.is_directory || target->second.entry.is_directory)
            throw Error(ErrorCode::AccessDenied,
                "replace requires two regular files");
        if (target->second.entry.attributes & kFileAttributeReadOnly)
            throw Error(ErrorCode::AccessDenied, "replace target is read-only");
        Bytes p;
        put8(p, 8);
        put_name(p, old_name);
        put_name(p, new_name);
        put64(p, old.id);
        put64(p, target->second.entry.id);
        put64(p, current_filetime());
        impl_->commit(p);
        return;
    }
    Bytes p;
    put8(p, 5);
    put_name(p, old_name);
    put_name(p, new_name);
    if (impl_->geometry.version == 6) put64(p, current_filetime());
    impl_->commit(p);
}
void Pool::SetBasicInfo(std::uint64_t id, std::uint32_t attributes,
                        std::uint64_t creation_time, std::uint64_t last_access_time,
                        std::uint64_t last_write_time, std::uint64_t change_time) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "persistent metadata requires v6");
    const auto& entry = impl_->find_by_id(id);
    if (attributes != kUnchangedAttributes) {
        attributes = normalized_attributes(attributes, entry.is_directory);
    }
    if (attributes == kUnchangedAttributes && !creation_time && !last_access_time &&
        !last_write_time && !change_time) return;
    Bytes p;
    put8(p, 6);
    put64(p, id);
    put32(p, attributes);
    put64(p, creation_time);
    put64(p, last_access_time);
    put64(p, last_write_time);
    put64(p, change_time);
    impl_->commit(p);
}
void Pool::Flush() {
    // Wait for an in-flight mutation. Each successful mutation has already
    // synchronized its data and mirrored journal before it returns.
    std::lock_guard lock(impl_->mutex);
}
std::uint64_t Pool::Capacity() const {
    std::lock_guard lock(impl_->mutex);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < impl_->disks.size(); ++i)
        total += impl_->data_end(i) - impl_->geometry.data_offset;
    return total;
}
std::uint64_t Pool::Available() const {
    std::lock_guard lock(impl_->mutex);
    std::uint64_t total = 0;
    for (std::size_t i = 0; i < impl_->disks.size(); ++i) {
        total += impl_->data_end(i) - impl_->data_tail[i];
        if (impl_->geometry.version >= 4) total += impl_->free_ranges[i].total;
    }
    return total;
}
bool Pool::RecoveredIncompleteTail() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->incomplete_tail;
}
std::uint64_t Pool::Scrub(const std::function<void(const ScrubIssue&)>& report) {
    std::lock_guard lock(impl_->mutex);
    if (impl_->geometry.version != 5 && impl_->geometry.version != 6)
        throw Error(ErrorCode::InvalidArgument, "scrub requires a v5 or v6 pool");
    if (impl_->incomplete_tail)
        throw Error(ErrorCode::Corrupt, "members have an incomplete final journal commit");
    std::uint64_t issue_count = 0;
    constexpr std::size_t kMaximumBatchBlocks = 256;
    const auto block = static_cast<std::uint64_t>(impl_->geometry.io_block);
    Bytes bytes;
    std::vector<std::size_t> damaged;
    for (const auto& [name, file] : impl_->files) {
        for (const auto& [logical, extent] : file.indexed) {
            const auto allocated = allocation_bytes(extent.length, impl_->geometry);
            for (std::uint64_t at = 0; at < allocated;) {
                const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(
                    kMaximumBatchBlocks, (allocated - at) / block));
                bytes.resize(count * static_cast<std::size_t>(block));
                damaged.clear();
                if (impl_->geometry.version == 6)
                    impl_->v6_read_blocks(extent.member, extent.physical + at, bytes, &damaged);
                else impl_->v5_read_blocks(extent.member, extent.physical + at, bytes, &damaged);
                for (const auto index : damaged) {
                    if (report) report(ScrubIssue{name, extent.member,
                        logical + at + index * block, extent.physical + at + index * block});
                    ++issue_count;
                }
                at += count * block;
            }
        }
    }
    return issue_count;
}

} // namespace poolfs
