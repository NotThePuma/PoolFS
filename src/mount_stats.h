#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <array>
#include <filesystem>
#include <optional>
#include <vector>

namespace poolfs::mount_stats {

struct Snapshot {
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t read_operations = 0;
    std::uint64_t write_operations = 0;
    std::uint64_t uptime_ms = 0;
    std::uint64_t capacity_bytes = 0;
    std::uint64_t available_bytes = 0;
};

struct MemberIdentity {
    std::uint64_t volume = 0;
    std::array<std::uint8_t, 16> index{};
    bool operator==(const MemberIdentity&) const = default;
};

struct MountedMembers {
    std::vector<MemberIdentity> identities;
    bool encrypted = false;
};

std::optional<MemberIdentity> IdentifyMember(const std::filesystem::path& path);
std::optional<MountedMembers> ReadMembers(wchar_t drive);

// Shared counters report completed filesystem reads/writes served to Windows.
// They do not include journal, checksum, checkpoint, or host-device I/O.
class Publisher {
public:
    Publisher() = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    ~Publisher();

    bool Start(wchar_t drive, SECURITY_ATTRIBUTES* security) noexcept;
    void PublishMembers(wchar_t drive, SECURITY_ATTRIBUTES* security,
                        const std::vector<std::filesystem::path>& paths, bool encrypted) noexcept;
    void RecordRead(std::uint64_t bytes) noexcept;
    void RecordWrite(std::uint64_t bytes) noexcept;

private:
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
    HANDLE members_mapping_ = nullptr;
};

Snapshot Read(wchar_t drive);

} // namespace poolfs::mount_stats
