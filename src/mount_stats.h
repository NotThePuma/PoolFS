#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>

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

// Shared counters report completed filesystem reads/writes served to Windows.
// They do not include journal, checksum, checkpoint, or host-device I/O.
class Publisher {
public:
    Publisher() = default;
    Publisher(const Publisher&) = delete;
    Publisher& operator=(const Publisher&) = delete;
    ~Publisher();

    bool Start(wchar_t drive, SECURITY_ATTRIBUTES* security) noexcept;
    void RecordRead(std::uint64_t bytes) noexcept;
    void RecordWrite(std::uint64_t bytes) noexcept;

private:
    HANDLE mapping_ = nullptr;
    void* view_ = nullptr;
};

Snapshot Read(wchar_t drive);

} // namespace poolfs::mount_stats
