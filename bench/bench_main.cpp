// PoolFS mount benchmark. Run only on a disposable mounted pool: v1-v3 deletion
// does not reclaim space, while v4 does. JSON goes to stdout; diagnostics to
// stderr. All test files are created with CREATE_NEW and use the given prefix.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>

#include "benchmark_io.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace poolfs::benchmark_io;

constexpr DWORD kMiB = 1024 * 1024;
constexpr DWORD kKiB = 1024;
constexpr int kOverwrites = 16;
constexpr int kSmallWrites = 128;
constexpr int kFlushes = 64;
constexpr int kMetadataFiles = 40;

double median(std::vector<double> values) {
    std::sort(values.begin(), values.end());
    const auto mid = values.size() / 2;
    return values.size() % 2 ? values[mid] : (values[mid - 1] + values[mid]) / 2.0;
}

std::wstring path_in(std::wstring directory, const std::wstring& filename) {
    if (directory.empty() || (directory.back() != L'\\' && directory.back() != L'/'))
        directory.push_back(L'\\');
    return directory + filename;
}

bool valid_prefix(std::wstring_view prefix) {
    if (prefix.empty() || prefix.size() > 40) return false;
    for (wchar_t c : prefix)
        if (!((c >= L'a' && c <= L'z') || (c >= L'A' && c <= L'Z') ||
              (c >= L'0' && c <= L'9') || c == L'-' || c == L'_'))
            return false;
    return true;
}

unsigned parse_size(std::wstring_view text) {
    if (text.empty()) throw std::invalid_argument("size MiB must be 1..32");
    unsigned value = 0;
    for (wchar_t c : text) {
        if (c < L'0' || c > L'9') throw std::invalid_argument("size MiB must be 1..32");
        value = value * 10u + static_cast<unsigned>(c - L'0');
        if (value > 32) throw std::invalid_argument("size MiB must be 1..32");
    }
    if (value < 1) throw std::invalid_argument("size MiB must be 1..32");
    return value;
}

struct Cleanup {
    std::vector<std::wstring> paths;
    ~Cleanup() {
        for (const auto& path : paths) {
            if (!DeleteFileW(path.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND)
                std::wcerr << L"Could not remove benchmark file: " << path << L'\n';
        }
    }
};

struct Metrics {
    double seq_write_mib_s = 0;
    double seq_read_mib_s = 0;
    double small_write_ms_mean = 0;
    double clean_flush_ms_mean = 0;
    double overwrite_read_ms_median = 0;
    double create_ms_mean = 0;
    double delete_ms_mean = 0;
};

Metrics run(const std::wstring& directory, const std::wstring& prefix,
            unsigned size_mib) {
    const DWORD attrs = GetFileAttributesW(directory.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || !(attrs & FILE_ATTRIBUTE_DIRECTORY))
        throw std::invalid_argument("directory does not exist");
    Clock clock;
    Cleanup cleanup;
    Buffer buffer(kMiB);
    auto* bytes = static_cast<unsigned char*>(buffer.get());
    std::fill_n(bytes, kMiB, 0x5a);
    const std::wstring seq_path = path_in(directory, prefix + L"-seq.bin");
    const std::wstring history_path = path_in(directory, prefix + L"-history.bin");
    Metrics metrics;

    // Time a sequential write followed by one explicit flush. PoolFS itself
    // currently syncs each Write callback; the explicit flush tests the API.
    {
        auto file = open_file(seq_path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW);
        cleanup.paths.push_back(seq_path);
        const auto start = clock.now();
        for (unsigned i = 0; i < size_mib; ++i) write_exact(file.get(), bytes, kMiB);
        flush(file.get());
        const double ms = clock.milliseconds(start);
        metrics.seq_write_mib_s = static_cast<double>(size_mib) / (ms / 1000.0);
    }

    // Noncached reads make the core's extent traversal visible rather than
    // measuring only Windows' top-level file cache.
    {
        auto file = open_file(seq_path, GENERIC_READ, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING |
                              FILE_FLAG_SEQUENTIAL_SCAN);
        const auto start = clock.now();
        for (unsigned i = 0; i < size_mib; ++i) {
            read_exact(file.get(), bytes, kMiB);
            if (bytes[0] != 0x5a || bytes[kMiB - 1] != 0x5a)
                throw std::runtime_error("sequential read verification failed");
        }
        const double ms = clock.milliseconds(start);
        metrics.seq_read_mib_s = static_cast<double>(size_mib) / (ms / 1000.0);
    }

    // Repeated small in-place writes quantify per-write journaling/sync cost.
    {
        auto file = open_file(seq_path, GENERIC_WRITE, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING);
        std::fill_n(bytes, 4 * kKiB, 0x7b);
        const auto start = clock.now();
        const auto sectors = static_cast<std::uint64_t>(size_mib) * 256;
        for (int i = 0; i < kSmallWrites; ++i) {
            const std::uint64_t sector = (static_cast<std::uint64_t>(i) * 193u) % sectors;
            seek(file.get(), sector * 4u * kKiB);
            write_exact(file.get(), bytes, 4 * kKiB);
        }
        metrics.small_write_ms_mean = clock.milliseconds(start) / kSmallWrites;
        const auto flush_start = clock.now();
        for (int i = 0; i < kFlushes; ++i) flush(file.get());
        metrics.clean_flush_ms_mean = clock.milliseconds(flush_start) / kFlushes;
    }

    // The final read should see only the newest 1 MiB extent. This isolates
    // the historical-extent read problem in the original core.
    {
        auto file = open_file(history_path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW);
        cleanup.paths.push_back(history_path);
        for (int i = 0; i <= kOverwrites; ++i) {
            std::fill_n(bytes, kMiB, static_cast<unsigned char>(0x20 + i));
            seek(file.get(), 0);
            write_exact(file.get(), bytes, kMiB);
        }
        flush(file.get());
    }
    {
        auto file = open_file(history_path, GENERIC_READ, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_NO_BUFFERING);
        std::vector<double> readings;
        readings.reserve(9);
        for (int i = 0; i < 9; ++i) {
            seek(file.get(), 0);
            const auto start = clock.now();
            read_exact(file.get(), bytes, kMiB);
            readings.push_back(clock.milliseconds(start));
            if (bytes[0] != 0x20 + kOverwrites ||
                bytes[kMiB - 1] != 0x20 + kOverwrites)
                throw std::runtime_error("overwrite read verification failed");
        }
        metrics.overwrite_read_ms_median = median(std::move(readings));
    }

    // Metadata work uses CREATE_NEW and deletes only names made by this run.
    std::vector<std::wstring> meta_paths;
    meta_paths.reserve(kMetadataFiles);
    const auto create_start = clock.now();
    for (int i = 0; i < kMetadataFiles; ++i) {
        auto path = path_in(directory, prefix + L"-meta-" + std::to_wstring(i));
        auto file = open_file(path, GENERIC_READ | GENERIC_WRITE, CREATE_NEW);
        cleanup.paths.push_back(path);
        meta_paths.push_back(std::move(path));
    }
    metrics.create_ms_mean = clock.milliseconds(create_start) / kMetadataFiles;
    const auto delete_start = clock.now();
    for (const auto& path : meta_paths)
        if (!DeleteFileW(path.c_str())) win_error("DeleteFileW");
    metrics.delete_ms_mean = clock.milliseconds(delete_start) / kMetadataFiles;
    return metrics;
}

} // namespace

int main() {
    int argc = 0;
    wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) return 1;
    if (argc != 3 && argc != 4) {
        std::cerr << "Usage: poolfs-bench <mounted-directory> <unique-prefix> [size-MiB: 1..32]\n";
        LocalFree(argv);
        return 2;
    }
    const std::wstring directory(argv[1]);
    const std::wstring prefix(argv[2]);
    const std::wstring size_text = argc == 4 ? argv[3] : L"8";
    LocalFree(argv);
    if (!valid_prefix(prefix)) {
        std::cerr << "Prefix must be 1..40 ASCII letters, digits, hyphens, or underscores.\n";
        return 2;
    }
    try {
        const unsigned size_mib = parse_size(size_text);
        const Metrics m = run(directory, prefix, size_mib);
        std::cout << std::fixed << std::setprecision(3)
                  << "{\"version\":1,\"size_mib\":" << size_mib
                  << ",\"seq_write_mib_s\":" << m.seq_write_mib_s
                  << ",\"seq_read_mib_s\":" << m.seq_read_mib_s
                  << ",\"small_write_ms_mean\":" << m.small_write_ms_mean
                  << ",\"clean_flush_ms_mean\":" << m.clean_flush_ms_mean
                  << ",\"overwrite_read_ms_median\":" << m.overwrite_read_ms_median
                  << ",\"create_ms_mean\":" << m.create_ms_mean
                  << ",\"delete_ms_mean\":" << m.delete_ms_mean << "}\n";
        std::cerr << std::fixed << std::setprecision(2)
                  << "Sequential write/read: " << m.seq_write_mib_s << "/"
                  << m.seq_read_mib_s << " MiB/s; small write/clean flush: "
                  << m.small_write_ms_mean << "/" << m.clean_flush_ms_mean
                  << " ms; overwritten read: " << m.overwrite_read_ms_median
                  << " ms; create/delete: " << m.create_ms_mean << "/"
                  << m.delete_ms_mean << " ms each\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "poolfs-bench: " << e.what() << '\n';
        return 1;
    }
}
