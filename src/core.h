#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace poolfs {

enum class ErrorCode {
    NotFound,
    AlreadyExists,
    NoSpace,
    InvalidName,
    Corrupt,
    Io,
    ReadOnly,
    AccessDenied,
    JournalFull,
    InvalidArgument,
};

class Error : public std::runtime_error {
public:
    Error(ErrorCode code, const std::string& message) : std::runtime_error(message), code_(code) {}
    ErrorCode code() const noexcept { return code_; }
private:
    ErrorCode code_;
};

// These on-disk formats are experimental and have no data redundancy. The
// The initial unencrypted layout is v6 revision 2; encrypted pools use v6
// revision 3. Both provide a directory tree with
// persistent basic metadata and stable file IDs. V1/v2 place each file on
// one member; v3-v6 can span members. V5/V6 checksum every 4 KiB data block
// for accidental damage. Journal and checkpoint metadata use FNV-1a, not
// authentication; encrypted pools authenticate only their public headers. Every
// write flushes data and the mirrored metadata journal before returning.
struct Entry {
    std::string name; // UTF-8 path, case-sensitive in this prototype.
    std::uint64_t size = 0;
    std::uint8_t member = 0;
    std::uint64_t id = 0;
    bool is_directory = false;
    std::uint32_t attributes = 0;
    std::uint64_t creation_time = 0;
    std::uint64_t last_access_time = 0;
    std::uint64_t last_write_time = 0;
    std::uint64_t change_time = 0;
};

struct ScrubIssue {
    std::string name;
    std::uint8_t member = 0;
    std::uint64_t logical = 0;
    std::uint64_t physical = 0;
};

// Raw members use GPT volume GUID paths. Both format versions verify the disk
// GUID; v3 also requires and verifies the selected partition's GUID.
struct RawMemberSpec {
    std::wstring volume_path;
    std::wstring expected_disk_guid;
    std::wstring expected_partition_guid = L"";
};

struct ImageMemberSpec {
    std::filesystem::path path;
    std::uint64_t bytes = 0;
};

struct ImageGrowSpec {
    std::filesystem::path source;
    std::filesystem::path destination;
    std::uint64_t bytes = 0;
};

class Pool {
public:
    struct V2ImageTag {};
    struct V3ImageTag {};
    struct V4ImageTag {};
    struct V5ImageTag {};
    struct V5ReadOnlyImageTag {};
    struct V6ImageTag {};
    struct V6ReadOnlyImageTag {};
    struct RawV3Tag {};
    static constexpr std::uint64_t kMinimumImageBytes = 8ULL * 1024 * 1024;
    static constexpr std::uint64_t kMinimumRawBytes = 256ULL * 1024 * 1024;
    static constexpr std::uint64_t kMinimumV4ImageBytes = 512ULL * 1024 * 1024;
    static constexpr std::uint64_t kMinimumV5ImageBytes = 384ULL * 1024 * 1024;
    static constexpr std::uint64_t kMinimumV6ImageBytes = 384ULL * 1024 * 1024;

    static void Format(const std::filesystem::path& first,
                       const std::filesystem::path& second,
                       std::uint64_t image_bytes);
    static void FormatV2Images(const std::filesystem::path& first,
                               const std::filesystem::path& second,
                               std::uint64_t image_bytes);
    static void FormatRaw(const RawMemberSpec& first, const RawMemberSpec& second);
    static void FormatV3Images(const std::vector<std::filesystem::path>& members,
                               std::uint64_t image_bytes);
    static void FormatV3Images(const std::vector<ImageMemberSpec>& members);
    static void FormatV4Images(const std::vector<std::filesystem::path>& members,
                               std::uint64_t image_bytes);
    static void FormatV4Images(const std::vector<ImageMemberSpec>& members);
    static void FormatV5Images(const std::vector<std::filesystem::path>& members,
                               std::uint64_t image_bytes);
    static void FormatV5Images(const std::vector<ImageMemberSpec>& members);
    static void FormatV6Images(const std::vector<std::filesystem::path>& members,
                               std::uint64_t image_bytes);
    static void FormatV6Images(const std::vector<ImageMemberSpec>& members);
    static void FormatV6EncryptedImages(const std::vector<std::filesystem::path>& members,
                                        std::uint64_t image_bytes,
                                        std::string_view password);
    static void FormatV6EncryptedImages(const std::vector<ImageMemberSpec>& members,
                                        std::string_view password);
    // Offline, copy-on-write expansion. Every source gets a new destination;
    // unchanged-size members are copied because every header has the manifest.
    static void GrowV6Images(const std::vector<ImageGrowSpec>& members,
                             std::string_view password = {});
    static bool V6Encrypted(const std::filesystem::path& member);
    static void FormatRawV3(const std::vector<RawMemberSpec>& members);

    Pool(const std::filesystem::path& first,
         const std::filesystem::path& second);
    Pool(V2ImageTag, const std::filesystem::path& first,
         const std::filesystem::path& second);
    Pool(const RawMemberSpec& first, const RawMemberSpec& second);
    Pool(V3ImageTag, const std::vector<std::filesystem::path>& members);
    Pool(V4ImageTag, const std::vector<std::filesystem::path>& members);
    Pool(V5ImageTag, const std::vector<std::filesystem::path>& members);
    Pool(V5ReadOnlyImageTag, const std::vector<std::filesystem::path>& members);
    Pool(V6ImageTag, const std::vector<std::filesystem::path>& members);
    Pool(V6ReadOnlyImageTag, const std::vector<std::filesystem::path>& members);
    Pool(V6ImageTag, const std::vector<std::filesystem::path>& members,
         std::string_view password);
    Pool(V6ReadOnlyImageTag, const std::vector<std::filesystem::path>& members,
         std::string_view password);
    Pool(RawV3Tag, const std::vector<RawMemberSpec>& members);
    ~Pool();
    Pool(const Pool&) = delete;
    Pool& operator=(const Pool&) = delete;

    std::vector<Entry> List() const;
    std::vector<Entry> ListDirectory(const std::string& directory) const;
    Entry Root() const;
    Entry Stat(const std::string& name) const;
    std::string NameForId(std::uint64_t id) const;
    void Create(const std::string& name, std::uint32_t attributes = 0);
    void MakeDirectory(const std::string& name, std::uint32_t attributes = 0);
    std::size_t Read(const std::string& name,
                     std::uint64_t offset,
                     std::span<std::byte> destination);
    std::size_t Write(const std::string& name,
                      std::uint64_t offset,
                      std::span<const std::byte> source);
    void Truncate(const std::string& name, std::uint64_t size);
    void Overwrite(const std::string& name, std::uint32_t attributes,
                   bool replace_attributes);
    void Remove(const std::string& name);
    bool IsDirectoryEmpty(const std::string& name) const;
    void Rename(const std::string& old_name, const std::string& new_name,
                bool replace_existing = false);
    void SetBasicInfo(std::uint64_t id, std::uint32_t attributes,
                      std::uint64_t creation_time, std::uint64_t last_access_time,
                      std::uint64_t last_write_time, std::uint64_t change_time);
    void Flush();

    std::uint64_t Capacity() const;
    std::uint64_t Available() const;
    bool RecoveredIncompleteTail() const;
    std::uint64_t Scrub(const std::function<void(const ScrubIssue&)>& report);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace poolfs
