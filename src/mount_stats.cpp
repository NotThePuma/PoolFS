#include "mount_stats.h"

#include <cstring>
#include <stdexcept>
#include <string>

namespace poolfs::mount_stats {
namespace {

constexpr DWORD kMagic = 0x53534650; // PFSS
constexpr DWORD kVersion = 1;

struct alignas(8) Shared {
    DWORD magic;
    DWORD version;
    ULONGLONG started_tick_ms;
    alignas(8) volatile LONG64 read_bytes;
    alignas(8) volatile LONG64 write_bytes;
    alignas(8) volatile LONG64 read_operations;
    alignas(8) volatile LONG64 write_operations;
};

std::wstring mapping_name(wchar_t drive) {
    if (drive >= L'a' && drive <= L'z') drive -= L'a' - L'A';
    if (drive < L'A' || drive > L'Z')
        throw std::invalid_argument("invalid PoolFS drive letter");
    return L"Local\\PoolFS.Stats." + std::wstring(1, drive);
}

} // namespace

Publisher::~Publisher() {
    if (view_) UnmapViewOfFile(view_);
    if (mapping_) CloseHandle(mapping_);
}

bool Publisher::Start(wchar_t drive, SECURITY_ATTRIBUTES* security) noexcept {
    if (mapping_ || view_) return false;
    try {
        const auto name = mapping_name(drive);
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, security,
            PAGE_READWRITE, 0, sizeof(Shared), name.c_str());
        if (!mapping_) return false;
        if (GetLastError() == ERROR_ALREADY_EXISTS) return false;
        view_ = MapViewOfFile(mapping_, FILE_MAP_WRITE, 0, 0, sizeof(Shared));
        if (!view_) return false;
        auto* shared = static_cast<Shared*>(view_);
        std::memset(shared, 0, sizeof(*shared));
        shared->version = kVersion;
        shared->started_tick_ms = GetTickCount64();
        MemoryBarrier();
        shared->magic = kMagic;
        return true;
    } catch (...) {
        return false;
    }
}

void Publisher::RecordRead(std::uint64_t bytes) noexcept {
    if (!view_ || !bytes) return;
    auto* shared = static_cast<Shared*>(view_);
    InterlockedAdd64(&shared->read_bytes, static_cast<LONG64>(bytes));
    InterlockedIncrement64(&shared->read_operations);
}

void Publisher::RecordWrite(std::uint64_t bytes) noexcept {
    if (!view_ || !bytes) return;
    auto* shared = static_cast<Shared*>(view_);
    InterlockedAdd64(&shared->write_bytes, static_cast<LONG64>(bytes));
    InterlockedIncrement64(&shared->write_operations);
}

Snapshot Read(wchar_t drive) {
    const auto name = mapping_name(drive);
    HANDLE mapping = OpenFileMappingW(FILE_MAP_READ, FALSE, name.c_str());
    if (!mapping)
        throw std::runtime_error(
            "no live statistics for this drive; mount it with the updated PoolFS");
    const void* view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, sizeof(Shared));
    if (!view) {
        CloseHandle(mapping);
        throw std::runtime_error("cannot read PoolFS statistics mapping");
    }
    const auto* shared = static_cast<const Shared*>(view);
    Snapshot result;
    if (shared->magic != kMagic || shared->version != kVersion) {
        UnmapViewOfFile(view);
        CloseHandle(mapping);
        throw std::runtime_error("unsupported PoolFS statistics mapping");
    }
    // These fields are 8-byte aligned; aligned reads are atomic on x64.
    result.read_bytes = static_cast<std::uint64_t>(shared->read_bytes);
    result.write_bytes = static_cast<std::uint64_t>(shared->write_bytes);
    result.read_operations = static_cast<std::uint64_t>(shared->read_operations);
    result.write_operations = static_cast<std::uint64_t>(shared->write_operations);
    result.uptime_ms = GetTickCount64() - shared->started_tick_ms;
    UnmapViewOfFile(view);
    CloseHandle(mapping);

    const auto root = std::wstring(1, drive) + L":\\";
    ULARGE_INTEGER available{}, total{}, free_total{};
    if (!GetDiskFreeSpaceExW(root.c_str(), &available, &total, &free_total))
        throw std::runtime_error("cannot read mounted PoolFS capacity");
    result.capacity_bytes = total.QuadPart;
    result.available_bytes = available.QuadPart;
    return result;
}

} // namespace poolfs::mount_stats
