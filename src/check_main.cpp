// Offline, read-only by default inspector for PoolFS v4/v6 image members.
// The repair mode discards only one cleanly interrupted final mirrored commit.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "core.h"
#include "crypto.h"
#include "password.h"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using Byte = std::uint8_t;
using Block = std::array<Byte, 4096>;
using Identity = std::array<Byte, 16>;
constexpr std::uint64_t kMiB = 1024ULL * 1024;
constexpr std::uint64_t kBlockBytes = 4096;
constexpr std::uint64_t kJournalStart = 2 * kBlockBytes;
constexpr std::uint64_t kEpochBytes = 32 * kMiB;
constexpr std::uint64_t kCheckpointStart = kJournalStart + 2 * kEpochBytes;
constexpr std::uint64_t kDataStart = 256 * kMiB;
constexpr std::uint64_t kCheckpointSlotBytes =
    ((kDataStart - kCheckpointStart) / 2 / kBlockBytes) * kBlockBytes;
constexpr std::uint64_t kMinimumV4MemberBytes = 512 * kMiB;
constexpr std::uint64_t kMinimumV6MemberBytes = 384 * kMiB;
constexpr std::size_t kMaximumMembers = 16;
constexpr std::size_t kManifestOffset = 64;
constexpr std::size_t kManifestEntryBytes = 24;
constexpr Byte kV6LayoutRevision = 2; // Member header byte 46.
constexpr Byte kV6EncryptedLayoutRevision = 3;
constexpr std::size_t kSaltOffset = 512;
constexpr std::size_t kSaltBytes = 32;
constexpr std::size_t kKdfIterationsOffset = 544;
constexpr std::size_t kHeaderMacOffset = 548;
constexpr std::size_t kHeaderMacBytes = 32;
constexpr std::size_t kRecordHeaderBytes = 20;
constexpr std::uint64_t kFnv64Offset = 14695981039346656037ULL;

class Corrupt final : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

std::uint32_t number32(const Byte* data) {
    std::uint32_t value = 0;
    for (int i = 0; i < 4; ++i) value |= std::uint32_t(data[i]) << (8 * i);
    return value;
}
std::uint64_t number64(const Byte* data) {
    std::uint64_t value = 0;
    for (int i = 0; i < 8; ++i) value |= std::uint64_t(data[i]) << (8 * i);
    return value;
}
void store32(Byte* data, std::uint32_t value) {
    for (int i = 0; i < 4; ++i) data[i] = static_cast<Byte>(value >> (8 * i));
}
void store64(Byte* data, std::uint64_t value) {
    for (int i = 0; i < 8; ++i) data[i] = static_cast<Byte>(value >> (8 * i));
}
std::uint32_t fnv32(const Byte* data, std::size_t count) {
    std::uint32_t hash = 2166136261U;
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= data[i];
        hash *= 16777619U;
    }
    return hash;
}
std::uint64_t fnv64_continue(std::uint64_t hash, const Byte* data, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        hash ^= data[i];
        hash *= 1099511628211ULL;
    }
    return hash;
}
std::uint64_t fnv64(const Byte* data, std::size_t count) {
    return fnv64_continue(kFnv64Offset, data, count);
}
bool all_zero(const Block& block) {
    return std::all_of(block.begin(), block.end(), [](Byte value) { return value == 0; });
}
std::string hex_id(const Identity& id) {
    std::ostringstream out;
    out << std::hex << std::setfill('0');
    for (const auto value : id) out << std::setw(2) << static_cast<unsigned>(value);
    return out.str();
}

class Image final {
public:
    Image(std::filesystem::path path, bool writable) : path_(std::move(path)), writable_(writable) {
        const DWORD access = GENERIC_READ | (writable ? GENERIC_WRITE : 0);
        handle_ = CreateFileW(path_.c_str(), access, 0, nullptr, OPEN_EXISTING,
                              FILE_FLAG_RANDOM_ACCESS, nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            throw std::runtime_error("cannot open member exclusively (unmount it first): " +
                                     path_.string() + " (Windows error " +
                                     std::to_string(GetLastError()) + ")");
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(handle_, &length) || length.QuadPart < 0) {
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
            throw std::runtime_error("cannot read member size: " + path_.string());
        }
        size_ = static_cast<std::uint64_t>(length.QuadPart);
    }
    ~Image() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    Image(const Image&) = delete;
    Image& operator=(const Image&) = delete;
    const std::filesystem::path& path() const { return path_; }
    std::uint64_t size() const { return size_; }
    void read_raw(std::uint64_t offset, Byte* bytes, std::size_t count) {
        if (offset > size_ || count > size_ - offset ||
            count > std::numeric_limits<DWORD>::max())
            throw Corrupt("member read exceeds image length: " + path_.string());
        seek(offset);
        DWORD received = 0;
        if (!ReadFile(handle_, bytes, static_cast<DWORD>(count), &received, nullptr) ||
            received != count)
            throw std::runtime_error("cannot read member: " + path_.string());
    }
    void enable_encryption(std::span<const std::byte, 64> key, std::uint8_t member_index) {
        if (cipher_) throw std::logic_error("member encryption already initialized");
        cipher_ = std::make_unique<poolfs::crypto::XtsCipher>(key);
        member_index_ = member_index;
    }
    bool encrypted() const { return bool(cipher_); }
    Block decrypt_raw_block(std::uint64_t offset, const Block& raw) const {
        if (!cipher_ || all_zero(raw)) return raw;
        Block plain{};
        const auto unit = (std::uint64_t(member_index_) << 56) | (offset / kBlockBytes);
        cipher_->decrypt_unit(unit, std::as_bytes(std::span(raw)),
                              std::as_writable_bytes(std::span(plain)));
        return plain;
    }
    void read(std::uint64_t offset, Byte* bytes, std::size_t count) {
        if (!cipher_ || offset < kJournalStart) {
            read_raw(offset, bytes, count);
            return;
        }
        if (offset > size_ || count > size_ - offset)
            throw Corrupt("member read exceeds image length: " + path_.string());
        while (count) {
            const auto block_offset = offset - offset % kBlockBytes;
            Block raw{};
            read_raw(block_offset, raw.data(), raw.size());
            const auto plain = decrypt_raw_block(block_offset, raw);
            const auto within = static_cast<std::size_t>(offset - block_offset);
            const auto portion = std::min(count, plain.size() - within);
            std::copy_n(plain.begin() + within, portion, bytes);
            offset += portion;
            bytes += portion;
            count -= portion;
        }
    }
    Block raw_block(std::uint64_t offset) {
        Block result{};
        read_raw(offset, result.data(), result.size());
        return result;
    }
    Block block(std::uint64_t offset) {
        Block result{};
        read(offset, result.data(), result.size());
        return result;
    }
    void write_raw_block(std::uint64_t offset, const Block& bytes) {
        if (!writable_) throw std::logic_error("read-only checker attempted a write");
        if (offset > size_ || kBlockBytes > size_ - offset)
            throw Corrupt("repair offset exceeds image length");
        seek(offset);
        DWORD written = 0;
        if (!WriteFile(handle_, bytes.data(), static_cast<DWORD>(bytes.size()), &written, nullptr) ||
            written != bytes.size())
            throw std::runtime_error("cannot write repair marker: " + path_.string());
        if (!FlushFileBuffers(handle_))
            throw std::runtime_error("cannot flush repaired member: " + path_.string());
    }
    void zero_block(std::uint64_t offset) { write_raw_block(offset, Block{}); }
private:
    void seek(std::uint64_t offset) {
        if (offset > static_cast<std::uint64_t>(std::numeric_limits<LONGLONG>::max()))
            throw Corrupt("member offset is not addressable");
        LARGE_INTEGER position{};
        position.QuadPart = static_cast<LONGLONG>(offset);
        if (!SetFilePointerEx(handle_, position, nullptr, FILE_BEGIN))
            throw std::runtime_error("cannot seek member: " + path_.string());
    }
    std::filesystem::path path_;
    bool writable_ = false;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::uint64_t size_ = 0;
    std::unique_ptr<poolfs::crypto::XtsCipher> cipher_;
    std::uint8_t member_index_ = 0;
};

struct ManifestEntry {
    Identity id{};
    std::uint64_t bytes = 0;
    bool operator==(const ManifestEntry&) const = default;
};
struct Header {
    std::uint8_t version = 0;
    std::uint8_t revision = 0;
    Identity pool_id{};
    Identity member_id{};
    std::uint8_t index = 0;
    std::uint8_t count = 0;
    std::uint64_t bytes = 0;
    std::vector<ManifestEntry> manifest;
    std::array<std::byte, kSaltBytes> salt{};
    std::uint32_t kdf_iterations = 0;
    std::array<std::byte, kHeaderMacBytes> header_mac{};
    bool encrypted() const { return version == 6 && revision == kV6EncryptedLayoutRevision; }
};

std::optional<Header> parse_header(const Block& block, std::uint64_t actual_size,
                                   std::string& reason) {
    constexpr std::array<Byte, 8> v4_magic{'P','O','O','L','F','S','0','4'};
    constexpr std::array<Byte, 8> v6_magic{'P','O','O','L','F','S','0','6'};
    const std::uint8_t version =
        std::equal(v4_magic.begin(), v4_magic.end(), block.begin()) ? 4 :
        std::equal(v6_magic.begin(), v6_magic.end(), block.begin()) ? 6 : 0;
    if (!version || number32(block.data() + 8) != version) {
        reason = "not a v4 or v6 member header";
        return std::nullopt;
    }
    const auto count = block[45];
    const auto index = block[44];
    if (count < (version == 6 ? 1 : 2) ||
        count > kMaximumMembers || index >= count) {
        reason = "invalid member count or index";
        return std::nullopt;
    }
    Block checksum_source = block;
    std::fill_n(checksum_source.begin() + 56, 4, 0);
    if (number32(block.data() + 56) != fnv32(checksum_source.data(), checksum_source.size())) {
        reason = "header checksum mismatch";
        return std::nullopt;
    }
    if (version == 6 && block[46] != kV6LayoutRevision &&
        block[46] != kV6EncryptedLayoutRevision) {
        reason = "unsupported earlier v6 layout";
        return std::nullopt;
    }
    Header header;
    header.version = version;
    header.revision = version == 6 ? block[46] : 0;
    std::copy_n(block.begin() + 12, header.pool_id.size(), header.pool_id.begin());
    std::copy_n(block.begin() + 28, header.member_id.size(), header.member_id.begin());
    header.index = index;
    header.count = count;
    header.bytes = number64(block.data() + 48);
    if (header.encrypted()) {
        header.kdf_iterations = number32(block.data() + kKdfIterationsOffset);
        if (header.kdf_iterations < 100000 || header.kdf_iterations > 10000000) {
            reason = "invalid encryption KDF cost";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < kSaltBytes; ++i)
            header.salt[i] = static_cast<std::byte>(block[kSaltOffset + i]);
        if (std::all_of(header.salt.begin(), header.salt.end(),
                        [](std::byte value) { return value == std::byte{0}; })) {
            reason = "invalid encryption salt";
            return std::nullopt;
        }
        for (std::size_t i = 0; i < kHeaderMacBytes; ++i)
            header.header_mac[i] = static_cast<std::byte>(block[kHeaderMacOffset + i]);
    }
    const auto minimum = version == 6 ? kMinimumV6MemberBytes : kMinimumV4MemberBytes;
    if (header.bytes < minimum || header.bytes != actual_size ||
        header.bytes % kBlockBytes != 0) {
        reason = "member file length disagrees with header";
        return std::nullopt;
    }
    for (std::size_t i = 0; i < count; ++i) {
        const auto base = kManifestOffset + i * kManifestEntryBytes;
        ManifestEntry entry;
        std::copy_n(block.begin() + base, entry.id.size(), entry.id.begin());
        entry.bytes = number64(block.data() + base + 16);
        if (entry.bytes < minimum || entry.bytes % kBlockBytes != 0 ||
            std::any_of(header.manifest.begin(), header.manifest.end(),
                        [&](const ManifestEntry& previous) { return previous.id == entry.id; })) {
            reason = "invalid or duplicate manifest entry";
            return std::nullopt;
        }
        header.manifest.push_back(entry);
    }
    if (header.manifest[index].id != header.member_id ||
        header.manifest[index].bytes != header.bytes) {
        reason = "member identity disagrees with manifest";
        return std::nullopt;
    }
    return header;
}

struct Member {
    std::unique_ptr<Image> image;
    Header header;
    Block raw_header{};
    unsigned valid_header_copies = 0;
};

Member read_member(std::unique_ptr<Image> image) {
    const Block raw0 = image->block(0);
    const Block raw1 = image->block(kBlockBytes);
    std::string reason0, reason1;
    const auto header0 = parse_header(raw0, image->size(), reason0);
    const auto header1 = parse_header(raw1, image->size(), reason1);
    if (!header0 && !header1)
        throw Corrupt("no valid v4/v6 header copies: " + reason0 + "; " + reason1);
    if (header0 && header1 && raw0 != raw1)
        throw Corrupt("member header copies disagree");
    Member member;
    member.image = std::move(image);
    member.header = header0 ? *header0 : *header1;
    member.raw_header = header0 ? raw0 : raw1;
    member.valid_header_copies = static_cast<unsigned>(bool(header0)) +
                                 static_cast<unsigned>(bool(header1));
    return member;
}

void unlock_members(std::vector<Member>& members,
                    const poolfs::crypto::KeyMaterial& keys) {
    const auto& first = members.front().header;
    if (!first.encrypted()) throw std::logic_error("unencrypted pool unlock requested");
    for (auto& member : members) {
        const auto& header = member.header;
        if (!header.encrypted() || header.salt != first.salt ||
            header.kdf_iterations != first.kdf_iterations)
            throw Corrupt("encrypted member KDF settings disagree");
        Block authenticated = member.raw_header;
        std::fill_n(authenticated.begin() + 56, 4, 0);
        std::fill_n(authenticated.begin() + kHeaderMacOffset, kHeaderMacBytes, 0);
        const auto expected = poolfs::crypto::hmac_sha256(
            keys.mac_key(), std::as_bytes(std::span(authenticated)));
        if (!poolfs::crypto::constant_time_equal(expected, header.header_mac))
            throw std::runtime_error("incorrect PoolFS password or damaged encrypted header");
        member.image->enable_encryption(keys.xts_key(), header.index);
    }
}

enum class CheckpointState { absent, invalid, valid };
struct Checkpoint {
    CheckpointState state = CheckpointState::absent;
    std::string reason;
    std::uint64_t generation = 0;
    std::uint64_t next_sequence = 0;
    std::uint8_t epoch = 0;
    std::uint64_t payload_length = 0;
    std::uint64_t payload_hash = 0;
};
std::uint64_t checkpoint_offset(std::uint8_t slot) {
    return kCheckpointStart + static_cast<std::uint64_t>(slot) * kCheckpointSlotBytes;
}
std::uint64_t epoch_offset(std::uint8_t epoch) {
    return kJournalStart + static_cast<std::uint64_t>(epoch) * kEpochBytes;
}

Checkpoint read_checkpoint(Image& image, std::uint8_t slot, std::uint8_t member_count,
                           std::uint8_t version) {
    Checkpoint cp;
    auto header = image.block(checkpoint_offset(slot));
    if (all_zero(header)) return cp;
    const std::array<Byte, 8> magic{'P','F','S','C','P','0','0',
                                    static_cast<Byte>('0' + version)};
    cp.state = CheckpointState::invalid;
    if (!std::equal(magic.begin(), magic.end(), header.begin())) {
        cp.reason = "bad checkpoint magic";
        return cp;
    }
    cp.generation = number64(header.data() + 8);
    cp.next_sequence = number64(header.data() + 16);
    cp.epoch = header[24];
    cp.payload_length = number64(header.data() + 32);
    cp.payload_hash = number64(header.data() + 40);
    const auto stored_header_hash = number64(header.data() + 48);
    std::fill_n(header.begin() + 48, 8, 0);
    if (!cp.generation || !cp.next_sequence || cp.epoch > 1 ||
        slot != (cp.generation - 1) % 2 || cp.epoch != (cp.generation - 1) % 2 ||
        cp.payload_length < 5 + 8 * member_count + 4 + (version == 6 ? 48 : 0) ||
        cp.payload_length > kCheckpointSlotBytes - kBlockBytes ||
        stored_header_hash != fnv64(header.data(), header.size())) {
        cp.reason = "checkpoint header is invalid";
        return cp;
    }
    std::array<Byte, 5> prefix{};
    image.read(checkpoint_offset(slot) + kBlockBytes, prefix.data(), prefix.size());
    if (prefix != std::array<Byte, 5>{'S','N','P',
                                      static_cast<Byte>('0' + version),member_count}) {
        cp.reason = "checkpoint payload prefix is invalid";
        return cp;
    }
    std::vector<Byte> chunk(1024 * 1024);
    std::uint64_t hash = kFnv64Offset;
    std::uint64_t position = 0;
    while (position < cp.payload_length) {
        const auto size = static_cast<std::size_t>(
            std::min<std::uint64_t>(chunk.size(), cp.payload_length - position));
        image.read(checkpoint_offset(slot) + kBlockBytes + position, chunk.data(), size);
        hash = fnv64_continue(hash, chunk.data(), size);
        position += size;
    }
    if (hash != cp.payload_hash) {
        cp.reason = "checkpoint payload checksum mismatch";
        return cp;
    }
    cp.state = CheckpointState::valid;
    return cp;
}

bool same_region(Image& left, Image& right, std::uint64_t offset, std::uint64_t length) {
    std::vector<Byte> a(1024 * 1024), b(1024 * 1024);
    std::uint64_t position = 0;
    while (position < length) {
        const auto size = static_cast<std::size_t>(
            std::min<std::uint64_t>(a.size(), length - position));
        left.read(offset + position, a.data(), size);
        right.read(offset + position, b.data(), size);
        if (!std::equal(a.begin(), a.begin() + size, b.begin())) return false;
        position += size;
    }
    return true;
}

bool same_checkpoint(Image& left_image, const Checkpoint& left,
                     Image& right_image, const Checkpoint& right, std::uint8_t slot) {
    if (left.state != CheckpointState::valid || right.state != CheckpointState::valid ||
        left.generation != right.generation || left.next_sequence != right.next_sequence ||
        left.epoch != right.epoch || left.payload_length != right.payload_length ||
        left.payload_hash != right.payload_hash) return false;
    return same_region(left_image, right_image,
                       checkpoint_offset(slot) + kBlockBytes, left.payload_length);
}

struct Journal {
    std::size_t count = 0;
    bool clean_end = false;
    std::string error;
};
std::string record_error(Block raw, std::uint64_t expected_sequence,
                         std::uint8_t version) {
    const std::array<Byte, 4> magic{'P','J','R',static_cast<Byte>('0' + version)};
    if (!std::equal(magic.begin(), magic.end(), raw.begin()))
        return "bad record magic";
    const auto length = number32(raw.data() + 4);
    if (length < kRecordHeaderBytes || length > kBlockBytes ||
        number64(raw.data() + 8) != expected_sequence)
        return "record length or sequence is invalid";
    const auto stored_hash = number32(raw.data() + 16);
    std::fill_n(raw.begin() + 16, 4, 0);
    if (stored_hash != fnv32(raw.data(), raw.size()) ||
        !std::all_of(raw.begin() + length, raw.end(),
                     [](Byte value) { return value == 0; }))
        return "record checksum or padding is invalid";
    return {};
}
Journal scan_journal(Image& image, const Checkpoint& checkpoint,
                     std::uint8_t version) {
    Journal scan;
    const auto begin = epoch_offset(checkpoint.epoch);
    for (std::size_t i = 0; i < kEpochBytes / kBlockBytes; ++i) {
        auto raw = image.block(begin + i * kBlockBytes);
        if (all_zero(raw)) {
            scan.clean_end = true;
            return scan;
        }
        if (checkpoint.next_sequence > std::numeric_limits<std::uint64_t>::max() - i) {
            scan.error = "record sequence overflows";
            return scan;
        }
        scan.error = record_error(raw, checkpoint.next_sequence + i, version);
        if (!scan.error.empty()) return scan;
        ++scan.count;
    }
    scan.error = "journal has no zero end marker";
    return scan;
}

enum class Status { clean, interrupted };
struct Report {
    Status status = Status::clean;
    std::vector<Member*> order;
    std::vector<Journal> journals;
    std::uint64_t generation = 0;
    std::uint64_t checkpoint_next_sequence = 0;
    std::uint8_t epoch = 0;
    std::size_t common_records = 0;
    std::size_t maximum_records = 0;
};

Report inspect(std::vector<Member>& members, bool output) {
    if (members.empty()) throw Corrupt("no readable members supplied");
    const auto& first = members.front().header;
    Report report;
    report.order.resize(first.count, nullptr);
    if (output) {
        std::cout << "format: v" << unsigned(first.version) << '\n';
        std::cout << "pool_id: " << hex_id(first.pool_id) << '\n';
        std::cout << "members: " << members.size() << '/' << unsigned(first.count) << '\n';
    }
    for (auto& member : members) {
        const auto& h = member.header;
        if (h.version != first.version || h.revision != first.revision ||
            h.pool_id != first.pool_id ||
            h.manifest != first.manifest ||
            h.count != first.count || h.index >= report.order.size())
            throw Corrupt("member belongs to a different pool or manifest");
        if (report.order[h.index]) throw Corrupt("duplicate member index supplied");
        report.order[h.index] = &member;
        if (output)
            std::cout << "  member[" << unsigned(h.index) << "]: " << member.image->path().string()
                      << ", bytes=" << h.bytes << ", header_copies="
                      << member.valid_header_copies << "/2\n";
    }
    for (std::size_t index = 0; index < report.order.size(); ++index) {
        if (!report.order[index]) {
            if (output)
                std::cout << "  missing[" << index << "]: expected_member_id="
                          << hex_id(first.manifest[index].id) << '\n';
            throw Corrupt("required pool member is missing");
        }
    }
    std::vector<std::array<Checkpoint, 2>> checkpoints(report.order.size());
    for (std::size_t index = 0; index < report.order.size(); ++index) {
        auto& member = *report.order[index];
        for (std::uint8_t slot = 0; slot < 2; ++slot) {
            auto& cp = checkpoints[index][slot];
            cp = read_checkpoint(*member.image, slot, first.count, first.version);
            if (output) {
                std::cout << "  checkpoint[" << index << "][" << unsigned(slot) << "]: ";
                if (cp.state == CheckpointState::valid)
                    std::cout << "valid generation=" << cp.generation << " epoch="
                              << unsigned(cp.epoch) << " next_sequence=" << cp.next_sequence;
                else if (cp.state == CheckpointState::absent)
                    std::cout << "absent";
                else
                    std::cout << "invalid (" << cp.reason << ')';
                std::cout << '\n';
            }
        }
    }
    std::optional<std::uint8_t> chosen_slot;
    for (std::uint8_t slot = 0; slot < 2; ++slot) {
        const auto& candidate = checkpoints[0][slot];
        if (candidate.state != CheckpointState::valid) continue;
        bool common = true;
        for (std::size_t index = 1; index < report.order.size(); ++index) {
            if (!same_checkpoint(*report.order[0]->image, candidate,
                                 *report.order[index]->image, checkpoints[index][slot], slot)) {
                common = false;
                break;
            }
        }
        if (common && (!chosen_slot ||
                       candidate.generation > checkpoints[0][*chosen_slot].generation))
            chosen_slot = slot;
    }
    if (!chosen_slot) throw Corrupt("no common valid checkpoint across all members");
    const auto& chosen = checkpoints[0][*chosen_slot];
    report.generation = chosen.generation;
    report.checkpoint_next_sequence = chosen.next_sequence;
    report.epoch = chosen.epoch;
    report.journals.resize(report.order.size());
    report.common_records = kEpochBytes / kBlockBytes;
    for (std::size_t index = 0; index < report.order.size(); ++index) {
        auto& journal = report.journals[index];
        journal = scan_journal(*report.order[index]->image, chosen, first.version);
        if (output)
            std::cout << "  journal[" << index << "]: records=" << journal.count
                      << ", end=" << (journal.clean_end ? "zero" : journal.error) << '\n';
        if (!journal.clean_end) throw Corrupt("member journal contains a damaged record or marker");
        report.common_records = std::min(report.common_records, journal.count);
        report.maximum_records = std::max(report.maximum_records, journal.count);
    }
    if (report.maximum_records - report.common_records > 1)
        throw Corrupt("journal copies differ by more than one final record");
    for (std::size_t position = 0; position < report.maximum_records; ++position) {
        std::optional<Block> expected;
        const auto offset = epoch_offset(chosen.epoch) + position * kBlockBytes;
        for (std::size_t index = 0; index < report.order.size(); ++index) {
            if (position >= report.journals[index].count) continue;
            auto raw = report.order[index]->image->block(offset);
            if (expected && raw != *expected)
                throw Corrupt("journal copies disagree at record " +
                              std::to_string(position));
            expected = std::move(raw);
        }
    }
    report.status = report.common_records == report.maximum_records
        ? Status::clean : Status::interrupted;
    if (output) {
        std::cout << "selected_checkpoint: generation=" << report.generation
                  << ", epoch=" << unsigned(report.epoch) << '\n';
        std::cout << "journal_common_records: " << report.common_records << '\n';
        if (report.status == Status::clean) {
            std::cout << "status: CLEAN\naction: none\n";
        } else {
            std::cout << "status: INTERRUPTED_FINAL_COMMIT\n"
                      << "action: mount read-only, or run --repair-final-tail to discard the "
                         "uncertain final operation\n";
        }
        std::cout << "scope: structural checks only; CLEAN does not verify file-data bytes "
                     "or all payload semantics\n";
    }
    return report;
}

// A committed sidecar is an undo record for a single 4 KiB journal slot.
// All sidecars are made durable before any member slot is changed. A staged
// .tmp file is never treated as committed and may be discarded on retry.
constexpr std::size_t kUndoBytes = 8192;
constexpr std::size_t kUndoRecordOffset = 80;
using UndoBytes = std::array<Byte, kUndoBytes>;
struct Undo {
    std::filesystem::path path;
    std::uint8_t version = 0;
    bool encrypted = false;
    Identity pool_id{};
    Identity member_id{};
    std::uint8_t member_index = 0;
    std::uint8_t epoch = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t offset = 0;
    Block record{};
};
std::filesystem::path undo_path(const Member& member) {
    return std::filesystem::path(member.image->path().wstring() + L".poolfs-check-undo");
}
std::filesystem::path temporary_undo_path(const Member& member) {
    return std::filesystem::path(undo_path(member).wstring() + L".tmp");
}
UndoBytes encode_undo(const Undo& undo) {
    UndoBytes bytes{};
    const std::array<Byte, 8> magic{'P','F','S','U','N','D','O',
                                    static_cast<Byte>('0' + undo.version)};
    std::copy(magic.begin(), magic.end(), bytes.begin());
    store32(bytes.data() + 8, undo.encrypted ? 2 : 1);
    std::copy(undo.pool_id.begin(), undo.pool_id.end(), bytes.begin() + 12);
    std::copy(undo.member_id.begin(), undo.member_id.end(), bytes.begin() + 28);
    bytes[44] = undo.member_index;
    bytes[45] = undo.epoch;
    bytes[46] = undo.encrypted ? 1 : 0;
    store64(bytes.data() + 48, undo.generation);
    store64(bytes.data() + 56, undo.sequence);
    store64(bytes.data() + 64, undo.offset);
    std::copy(undo.record.begin(), undo.record.end(), bytes.begin() + kUndoRecordOffset);
    store64(bytes.data() + 72, fnv64(bytes.data(), bytes.size()));
    return bytes;
}
Undo decode_undo(const std::filesystem::path& path) {
    Image file(path, false);
    if (file.size() != kUndoBytes) throw Corrupt("undo sidecar has wrong length: " + path.string());
    UndoBytes bytes{};
    file.read(0, bytes.data(), bytes.size());
    constexpr std::array<Byte, 7> magic_prefix{'P','F','S','U','N','D','O'};
    const std::uint8_t version = bytes[7] == '4' ? 4 : bytes[7] == '6' ? 6 : 0;
    const auto sidecar_format = number32(bytes.data() + 8);
    const bool encrypted = sidecar_format == 2 && bytes[46] == 1 && version == 6;
    if (!version || !std::equal(magic_prefix.begin(), magic_prefix.end(), bytes.begin()) ||
        !((sidecar_format == 1 && bytes[46] == 0) || encrypted) || bytes[47] ||
        !std::all_of(bytes.begin() + kUndoRecordOffset + kBlockBytes, bytes.end(),
                     [](Byte value) { return value == 0; }))
        throw Corrupt("undo sidecar header or padding is invalid: " + path.string());
    const auto stored_hash = number64(bytes.data() + 72);
    std::fill_n(bytes.begin() + 72, 8, 0);
    if (stored_hash != fnv64(bytes.data(), bytes.size()))
        throw Corrupt("undo sidecar checksum mismatch: " + path.string());
    Undo undo;
    undo.path = path;
    undo.version = version;
    undo.encrypted = encrypted;
    std::copy_n(bytes.begin() + 12, undo.pool_id.size(), undo.pool_id.begin());
    std::copy_n(bytes.begin() + 28, undo.member_id.size(), undo.member_id.begin());
    undo.member_index = bytes[44];
    undo.epoch = bytes[45];
    undo.generation = number64(bytes.data() + 48);
    undo.sequence = number64(bytes.data() + 56);
    undo.offset = number64(bytes.data() + 64);
    std::copy_n(bytes.begin() + kUndoRecordOffset, undo.record.size(), undo.record.begin());
    if (!undo.generation || undo.epoch > 1 ||
        (!undo.encrypted &&
         !record_error(undo.record, undo.sequence, undo.version).empty()))
        throw Corrupt("undo sidecar contains an invalid journal record: " + path.string());
    return undo;
}

void delete_file(const std::filesystem::path& path) {
    if (!DeleteFileW(path.c_str()))
        throw std::runtime_error("cannot remove repair sidecar: " + path.string() +
                                 " (Windows error " + std::to_string(GetLastError()) + ")");
}
void prepare_undo(const Undo& undo) {
    const auto temp = std::filesystem::path(undo.path.wstring() + L".tmp");
    const UndoBytes bytes = encode_undo(undo);
    HANDLE handle = CreateFileW(temp.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                CREATE_NEW, FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH,
                                nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot stage repair sidecar: " + temp.string());
    DWORD written = 0;
    const bool success = WriteFile(handle, bytes.data(), static_cast<DWORD>(bytes.size()),
                                   &written, nullptr) && written == bytes.size() &&
                         FlushFileBuffers(handle);
    CloseHandle(handle);
    if (!success)
        throw std::runtime_error("cannot durably write repair sidecar: " + temp.string());
    if (!MoveFileExW(temp.c_str(), undo.path.c_str(), MOVEFILE_WRITE_THROUGH))
        throw std::runtime_error("cannot commit repair sidecar: " + undo.path.string());
    const auto verified = decode_undo(undo.path);
    if (verified.version != undo.version || verified.encrypted != undo.encrypted ||
        verified.pool_id != undo.pool_id ||
        verified.member_id != undo.member_id ||
        verified.generation != undo.generation || verified.sequence != undo.sequence ||
        verified.offset != undo.offset || verified.record != undo.record)
        throw Corrupt("committed repair sidecar failed verification");
}

std::vector<Undo> pending_undos(const std::vector<Member>& members) {
    std::vector<Undo> pending;
    for (const auto& member : members) {
        const auto path = undo_path(member);
        if (!std::filesystem::exists(path)) continue;
        auto undo = decode_undo(path);
        if (undo.version != member.header.version ||
            undo.encrypted != member.header.encrypted() ||
            undo.pool_id != member.header.pool_id ||
            undo.member_id != member.header.member_id ||
            undo.member_index != member.header.index)
            throw Corrupt("repair sidecar does not match its member: " + path.string());
        pending.push_back(std::move(undo));
    }
    return pending;
}

Block undo_plaintext(const Undo& undo, const std::vector<Member>& members) {
    const auto it = std::find_if(members.begin(), members.end(), [&](const Member& member) {
        return member.header.index == undo.member_index &&
               member.header.member_id == undo.member_id;
    });
    if (it == members.end()) throw Corrupt("repair sidecar member is missing");
    const auto plain = it->image->decrypt_raw_block(undo.offset, undo.record);
    if (!record_error(plain, undo.sequence, undo.version).empty())
        throw Corrupt("repair sidecar contains an invalid journal record: " +
                      undo.path.string());
    return plain;
}

void discard_staged_sidecars(const std::vector<Member>& members) {
    for (const auto& member : members) {
        const auto path = temporary_undo_path(member);
        if (std::filesystem::exists(path)) delete_file(path);
    }
}

void validate_pending_undos(const std::vector<Member>& members,
                            const std::vector<Undo>& pending) {
    if (pending.empty()) return;
    const auto& first = pending.front();
    const auto first_plain = undo_plaintext(first, members);
    if (members.size() != members.front().header.count)
        throw Corrupt("repair sidecars require every pool member");
    std::vector<bool> seen(members.front().header.count);
    for (const auto& member : members) {
        if (member.header.index >= seen.size() || seen[member.header.index] ||
            member.header.version != members.front().header.version ||
            member.header.revision != members.front().header.revision ||
            member.header.pool_id != members.front().header.pool_id ||
            member.header.manifest != members.front().header.manifest ||
            member.header.revision != members.front().header.revision)
            throw Corrupt("repair sidecars have a missing, duplicate, or foreign member");
        seen[member.header.index] = true;
    }
    const auto slot = static_cast<std::uint8_t>((first.generation - 1) % 2);
    const auto first_checkpoint = read_checkpoint(*members.front().image, slot,
                                                   members.front().header.count,
                                                   members.front().header.version);
    if (first_checkpoint.state != CheckpointState::valid ||
        first_checkpoint.generation != first.generation ||
        first_checkpoint.epoch != first.epoch)
        throw Corrupt("repair sidecar checkpoint is not currently authoritative");
    for (const auto& member : members) {
        if (member.header.version != first.version ||
            member.header.pool_id != first.pool_id ||
            member.header.manifest != members.front().header.manifest)
            throw Corrupt("repair sidecars do not match a complete pool");
        const auto checkpoint = read_checkpoint(*member.image, slot,
                                                member.header.count, member.header.version);
        if (!same_checkpoint(*members.front().image, first_checkpoint,
                             *member.image, checkpoint, slot))
            throw Corrupt("repair sidecar checkpoint differs across members");
        const auto newer = read_checkpoint(*member.image,
            static_cast<std::uint8_t>(1 - slot), member.header.count,
            member.header.version);
        if (newer.state == CheckpointState::valid &&
            newer.generation > first.generation)
            throw Corrupt("repair sidecar is stale after a newer checkpoint");
    }
    if (first.sequence < first_checkpoint.next_sequence ||
        first.sequence - first_checkpoint.next_sequence >= kEpochBytes / kBlockBytes - 1 ||
        first.offset != epoch_offset(first.epoch) +
            (first.sequence - first_checkpoint.next_sequence) * kBlockBytes)
        throw Corrupt("repair sidecar journal offset or sequence is invalid");
    for (std::uint64_t position = 0;
         position < first.sequence - first_checkpoint.next_sequence; ++position) {
        std::optional<Block> expected;
        for (const auto& member : members) {
            const auto raw = member.image->block(epoch_offset(first.epoch) +
                                                 position * kBlockBytes);
            if (!record_error(raw, first_checkpoint.next_sequence + position,
                              first.version).empty() ||
                (expected && raw != *expected))
                throw Corrupt("repair sidecar has a damaged or divergent earlier journal record");
            expected = raw;
        }
    }
    for (const auto& member : members) {
        if (!all_zero(member.image->block(first.offset + kBlockBytes)))
            throw Corrupt("repair sidecar is stale: later journal data exists");
        const bool has_undo = std::any_of(pending.begin(), pending.end(),
            [&](const Undo& undo) { return undo.member_index == member.header.index; });
        if (!has_undo) {
            const auto current = member.image->block(first.offset);
            if (current != first_plain && !all_zero(current))
                throw Corrupt("journal changed on a member without an undo sidecar");
        }
    }
    for (const auto& undo : pending) {
        if (undo.version != first.version || undo.pool_id != first.pool_id ||
            undo.generation != first.generation ||
            undo.epoch != first.epoch || undo.sequence != first.sequence ||
            undo.offset != first.offset ||
            undo_plaintext(undo, members) != first_plain)
            throw Corrupt("repair sidecars disagree about the interrupted record");
        auto it = std::find_if(members.begin(), members.end(), [&](const Member& member) {
            return member.header.index == undo.member_index &&
                   member.header.member_id == undo.member_id;
        });
        if (it == members.end()) throw Corrupt("repair sidecar member is missing");
        const auto current = it->image->raw_block(undo.offset);
        for (std::size_t i = 0; i < current.size(); ++i)
            if (current[i] != undo.record[i] && current[i] != 0)
                throw Corrupt("repair sidecar target changed unexpectedly; refusing restore");
    }
}

void resume_pending_undos(std::vector<Member>& members,
                          const std::vector<Undo>& pending) {
    if (pending.empty()) return;
    validate_pending_undos(members, pending);
    for (const auto& undo : pending) {
        auto it = std::find_if(members.begin(), members.end(), [&](const Member& member) {
            return member.header.index == undo.member_index;
        });
        it->image->write_raw_block(undo.offset, undo.record);
    }
    // Keep all sidecars until every original record is durably restored and
    // the resulting pool can be structurally inspected again.
    inspect(members, false);
    for (const auto& undo : pending) delete_file(undo.path);
    std::cout << "recovery: restored " << pending.size()
              << " original journal record(s) from durable sidecars\n";
}

int main_impl(int argc, char** argv) {
    const bool repair = argc >= 2 && std::string_view(argv[1]) == "--repair-final-tail";
    const int first_path = repair ? 2 : 1;
    const int count = argc - first_path;
    if (count < 1 || count > static_cast<int>(kMaximumMembers)) {
        std::cerr << "Usage: poolfs-check <member.img> [up to 16 members]\n"
                  << "   or: poolfs-check --repair-final-tail <member1.img> <member2.img> "
                     "[up to 16 members; mirrored pools only]\n";
        return 1;
    }
    std::vector<std::filesystem::path> paths;
    std::vector<Member> members;
    paths.reserve(count);
    members.reserve(count);
    bool io_problem = false;
    bool corrupt_problem = false;
    std::cout << "poolfs-check: " << (repair ? "offline repair inspection" :
                                    "offline read-only inspection") << '\n';
    for (int i = first_path; i < argc; ++i) {
        const std::filesystem::path path =
            std::filesystem::absolute(std::filesystem::path(argv[i])).lexically_normal();
        paths.push_back(path);
        try {
            members.push_back(read_member(std::make_unique<Image>(path, repair)));
        } catch (const Corrupt& error) {
            std::cout << "  supplied[" << (i - first_path) << "]: " << path.string()
                      << " (invalid: " << error.what() << ")\n";
            corrupt_problem = true;
        } catch (const std::exception& error) {
            std::cout << "  supplied[" << (i - first_path) << "]: " << path.string()
                      << " (unavailable: " << error.what() << ")\n";
            io_problem = true;
        }
    }
    if (io_problem)
        throw std::runtime_error("one or more members could not be opened exclusively; "
                                 "unmount them and retry; no changes made");
    if (corrupt_problem)
        throw Corrupt("one or more supplied member headers are invalid; no changes made");
    for (const auto& member : members)
        if (member.header.version != members.front().header.version ||
            member.header.revision != members.front().header.revision)
            throw Corrupt("member format revisions disagree; no changes made");
    if (repair && members.front().header.count == 1)
        throw Corrupt("single-member pools have no mirrored journal copy; "
                      "automatic final-tail repair is unavailable and no changes were made");
    std::optional<poolfs::password::Secret> secret;
    if (members.front().header.encrypted()) {
        secret.emplace(poolfs::password::prompt(L"PoolFS password: "));
        const auto& header = members.front().header;
        const auto keys = poolfs::crypto::derive_keys(
            secret->view(), header.salt, header.kdf_iterations);
        unlock_members(members, keys);
    }
    const auto pending = pending_undos(members);
    if (!repair && !pending.empty()) {
        if (members.front().header.count == 1)
            std::cout << "status: PENDING_REPAIR\naction: preserve the sidecar and member; "
                         "automatic single-member repair is unavailable\n";
        else
            std::cout << "status: PENDING_REPAIR\naction: run --repair-final-tail offline "
                         "to restore durable undo sidecars before mounting\n";
        return 3;
    }
    if (repair) {
        resume_pending_undos(members, pending);
        // A staged .tmp file alone cannot precede a member mutation: the
        // committed sidecars for every target are created before zeroing.
        discard_staged_sidecars(members);
    }
    auto report = inspect(members, true);
    if (!repair) return report.status == Status::clean ? 0 : 2;
    if (report.status != Status::interrupted)
        throw Corrupt("--repair-final-tail requires exactly one clean extra final record; "
                      "no changes made");
    const auto extra_offset = epoch_offset(report.epoch) + report.common_records * kBlockBytes;
    if (report.checkpoint_next_sequence >
        std::numeric_limits<std::uint64_t>::max() - report.common_records)
        throw Corrupt("interrupted journal sequence overflows");
    const auto extra_sequence = report.checkpoint_next_sequence + report.common_records;
    std::cout << "repair: discarding the uncertain final operation at journal offset "
              << extra_offset << "\n";
    std::vector<Undo> prepared;
    for (std::size_t index = 0; index < report.order.size(); ++index) {
        if (report.journals[index].count != report.maximum_records) continue;
        auto& member = *report.order[index];
        Undo undo;
        undo.path = undo_path(member);
        undo.version = member.header.version;
        undo.encrypted = member.header.encrypted();
        undo.pool_id = member.header.pool_id;
        undo.member_id = member.header.member_id;
        undo.member_index = member.header.index;
        undo.epoch = report.epoch;
        undo.generation = report.generation;
        undo.sequence = extra_sequence;
        undo.offset = extra_offset;
        undo.record = member.image->raw_block(extra_offset);
        if (!record_error(member.image->block(extra_offset), extra_sequence,
                          undo.version).empty())
            throw Corrupt("extra final record changed before repair");
        prepare_undo(undo);
        prepared.push_back(std::move(undo));
    }
    std::size_t zeroed = 0;
    for (std::size_t index = 0; index < report.order.size(); ++index) {
        if (report.journals[index].count == report.maximum_records) {
            report.order[index]->image->zero_block(extra_offset);
            ++zeroed;
            char test_exit[2]{};
            if (zeroed == 1 &&
                GetEnvironmentVariableA("POOLFS_CHECK_TEST_EXIT_AFTER_FIRST_ZERO",
                                        test_exit, sizeof(test_exit)) == 1 &&
                test_exit[0] == '1')
                ExitProcess(89);
        }
    }
    auto verified = inspect(members, false);
    if (verified.status != Status::clean || verified.generation != report.generation ||
        verified.epoch != report.epoch || verified.common_records != report.common_records)
        throw Corrupt("repair did not produce a clean matching journal; inspect again");
    for (const auto& undo : prepared) delete_file(undo.path);
    const auto version = members.front().header.version;
    const bool encrypted = members.front().header.encrypted();
    members.clear(); // Release exclusive handles before the normal PoolFS open.
    if (version == 6) {
        if (encrypted) {
            poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, paths, secret->view());
            if (pool.RecoveredIncompleteTail())
                throw Corrupt("normal v6 open remains read-only after repair");
        } else {
            poolfs::Pool pool(poolfs::Pool::V6ImageTag{}, paths);
            if (pool.RecoveredIncompleteTail())
                throw Corrupt("normal v6 open remains read-only after repair");
        }
    } else {
        poolfs::Pool pool(poolfs::Pool::V4ImageTag{}, paths);
        if (pool.RecoveredIncompleteTail())
            throw Corrupt("normal v4 open remains read-only after repair");
    }
    std::cout << "status: REPAIRED\naction: uncertain final operation discarded; "
                 "normal v" << unsigned(version) << " open succeeded\n";
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    try {
        return main_impl(argc, argv);
    } catch (const Corrupt& error) {
        std::cerr << "status: CORRUPT\nreason: " << error.what() << '\n';
        return 3;
    } catch (const std::exception& error) {
        std::cerr << "status: ERROR\nreason: " << error.what() << '\n';
        return 1;
    }
}
