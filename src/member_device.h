#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace poolfs {

// Positional storage used by the PoolFS core. Raw members require aligned
// writes; their implementation may use an aligned bounce buffer for reads.
class MemberDevice {
public:
    virtual ~MemberDevice() = default;
    virtual std::uint64_t size() const = 0;
    virtual void read(std::uint64_t offset, std::byte* data, std::size_t length) = 0;
    virtual void write(std::uint64_t offset, const std::byte* data, std::size_t length) = 0;
    virtual void sync() = 0;
};

struct RawVolumeInfo {
    std::wstring volume_path;
    std::wstring disk_guid;
    std::uint64_t bytes = 0; // Usable bytes; final 16 MiB of the partition is reserved.
    std::uint32_t disk_number = 0;
    std::uint32_t logical_sector_bytes = 0;
    std::uint32_t physical_sector_bytes = 0;
    std::wstring partition_guid;
    std::uint64_t partition_start_bytes = 0;
    std::uint64_t partition_length_bytes = 0;
};

// A dedicated GPT basic-data partition opened by its \\?\Volume{...} path.
// The constructor requires an exact expected GPT disk GUID and holds an
// exclusive volume lock for its lifetime. It rejects boot, EFI, and page-file
// disks. Without an expected partition GUID (v2), the disk must have exactly
// one basic-data partition. With one (v3), the selected partition must match.
class RawVolume final : public MemberDevice {
public:
    RawVolume(const std::wstring& volume_path,
              const std::wstring& expected_disk_guid,
              const std::wstring& expected_partition_guid = L"");
    ~RawVolume() override;
    RawVolume(const RawVolume&) = delete;
    RawVolume& operator=(const RawVolume&) = delete;

    const RawVolumeInfo& info() const noexcept;
    std::uint64_t size() const override;
    void read(std::uint64_t offset, std::byte* data, std::size_t length) override;
    void write(std::uint64_t offset, const std::byte* data, std::size_t length) override;
    void sync() override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace poolfs
