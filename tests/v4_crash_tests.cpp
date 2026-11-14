#include "core.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
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
constexpr DWORD kInjectedCrashExit = 86;
constexpr DWORD kChildTimeoutMs = 120000;
constexpr int kMaximumIoPoints = 128;
constexpr std::string_view kAnchor = "acknowledged before injected crash";
constexpr std::string_view kFaultEnv = "POOLFS_TEST_V4_CRASH_AT";

enum class Scenario { Create, Write, Overwrite, Replace, Delete, Rotation };
enum class Outcome { Old, New, FailClosed };
#ifdef POOLFS_CRASH_V6_ENCRYPTED
using CrashImageTag = Pool::V6ImageTag;
constexpr std::string_view kFormatName = "v6-encrypted";
constexpr std::string_view kTestPassword = "PoolFS-crash-test-password-only-2026";
constexpr std::array kScenarios{Scenario::Write, Scenario::Overwrite,
                                Scenario::Rotation};
#elif defined(POOLFS_CRASH_V6)
using CrashImageTag = Pool::V6ImageTag;
constexpr std::string_view kFormatName = "v6";
constexpr std::array kScenarios{Scenario::Write, Scenario::Overwrite,
                                Scenario::Replace, Scenario::Rotation};
#elif defined(POOLFS_CRASH_V5)
using CrashImageTag = Pool::V5ImageTag;
constexpr std::string_view kFormatName = "v5";
constexpr std::array kScenarios{Scenario::Write, Scenario::Overwrite};
#else
using CrashImageTag = Pool::V4ImageTag;
constexpr std::string_view kFormatName = "v4";
constexpr std::array kScenarios{Scenario::Create, Scenario::Write,
                                Scenario::Overwrite, Scenario::Delete, Scenario::Rotation};
#endif

void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

std::string scenario_name(Scenario scenario) {
    switch (scenario) {
    case Scenario::Create: return "create";
    case Scenario::Write: return "write";
    case Scenario::Overwrite: return "overwrite";
    case Scenario::Replace: return "replace";
    case Scenario::Delete: return "delete";
    case Scenario::Rotation: return "rotation";
    }
    throw std::runtime_error("unknown scenario");
}

Scenario parse_scenario(std::string_view text) {
    for (const auto scenario : {Scenario::Create, Scenario::Write,
                                Scenario::Overwrite, Scenario::Replace,
                                Scenario::Delete, Scenario::Rotation})
        if (text == scenario_name(scenario)) return scenario;
    throw std::runtime_error("unknown scenario: " + std::string(text));
}

std::span<const std::byte> bytes(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()), text.size()};
}

std::vector<std::byte> write_payload() {
    std::vector<std::byte> result(8192);
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = static_cast<std::byte>((i * 29 + 7) % 251);
    return result;
}

std::vector<fs::path> members(const std::array<fs::path, 2>& pair) {
    return {pair[0], pair[1]};
}

struct CrashPool final : Pool {
    explicit CrashPool(const std::vector<fs::path>& images)
#ifdef POOLFS_CRASH_V6_ENCRYPTED
        : Pool(CrashImageTag{}, images, kTestPassword) {}
#else
        : Pool(CrashImageTag{}, images) {}
#endif
};

void seed(Scenario scenario, const std::array<fs::path, 2>& pair) {
#ifdef POOLFS_CRASH_V6_ENCRYPTED
    Pool::FormatV6EncryptedImages(members(pair), kMemberBytes, kTestPassword);
#elif defined(POOLFS_CRASH_V6)
    Pool::FormatV6Images(members(pair), kMemberBytes);
#elif defined(POOLFS_CRASH_V5)
    Pool::FormatV5Images(members(pair), kMemberBytes);
#else
    Pool::FormatV4Images(members(pair), kMemberBytes);
#endif
    CrashPool pool(members(pair));
    pool.Create("anchor.bin");
    require(pool.Write("anchor.bin", 0, bytes(kAnchor)) == kAnchor.size(),
            "could not seed anchor");
    switch (scenario) {
    case Scenario::Create:
        break;
    case Scenario::Write:
        pool.Create("target.bin");
        break;
    case Scenario::Overwrite: {
        pool.Create("target.bin");
        std::vector<std::byte> original(8192, std::byte{'A'});
        require(pool.Write("target.bin", 0, original) == original.size(),
                "could not seed overwrite target");
        pool.Create("reclaim.bin");
        std::vector<std::byte> scratch(8192, std::byte{'S'});
        require(pool.Write("reclaim.bin", 0, scratch) == scratch.size(),
                "could not seed reclaim space");
        pool.Remove("reclaim.bin");
        break;
    }
    case Scenario::Replace: {
        pool.Create("source.bin");
        pool.Create("target.bin");
        const std::vector<std::byte> source(8192, std::byte{'S'});
        const std::vector<std::byte> target(8192, std::byte{'T'});
        require(pool.Write("source.bin", 0, source) == source.size(),
                "could not seed replace source");
        require(pool.Write("target.bin", 0, target) == target.size(),
                "could not seed replace target");
        break;
    }
    case Scenario::Delete: {
        pool.Create("target.bin");
        std::vector<std::byte> original(8192, std::byte{'D'});
        require(pool.Write("target.bin", 0, original) == original.size(),
                "could not seed delete target");
        break;
    }
    case Scenario::Rotation:
        // A 32 MiB journal epoch has 8,191 usable 4 KiB record slots.
        // Anchor create/write use two; these pairs use 8,188 more.
        for (int i = 0; i < 4094; ++i) {
            pool.Create("churn.bin");
            pool.Remove("churn.bin");
        }
        pool.Create("rotating.bin"); // Record 8,191: next commit rotates.
        break;
    }
}

void run_operation(Scenario scenario, const std::array<fs::path, 2>& pair) {
    CrashPool pool(members(pair));
    switch (scenario) {
    case Scenario::Create:
        pool.Create("target.bin");
        break;
    case Scenario::Write: {
        const auto payload = write_payload();
        require(pool.Write("target.bin", 0, payload) == payload.size(),
                "child write returned short count");
        break;
    }
    case Scenario::Overwrite: {
        const std::array<std::byte, 3> replacement{
            std::byte{'B'}, std::byte{'B'}, std::byte{'B'}};
        require(pool.Write("target.bin", 4095, replacement) == replacement.size(),
                "child overwrite returned short count");
        break;
    }
    case Scenario::Replace:
        pool.Rename("source.bin", "target.bin", true);
        break;
    case Scenario::Delete:
        pool.Remove("target.bin");
        break;
    case Scenario::Rotation:
        pool.Remove("rotating.bin");
        break;
    }
}

bool exists(Pool& pool, const std::string& name) {
    try {
        pool.Stat(name);
        return true;
    } catch (const poolfs::Error& error) {
        if (error.code() == poolfs::ErrorCode::NotFound) return false;
        throw;
    }
}

std::vector<std::byte> read_all(Pool& pool, const std::string& name) {
    const auto size = pool.Stat(name).size;
    require(size <= 8192, "unexpected test file length");
    std::vector<std::byte> result(static_cast<std::size_t>(size));
    require(pool.Read(name, 0, result) == result.size(), "short test file read");
    return result;
}

Outcome verify(Scenario scenario, const std::array<fs::path, 2>& pair,
               bool require_new) {
    try {
        CrashPool pool(members(pair));
        const auto anchor = read_all(pool, "anchor.bin");
        require(std::equal(anchor.begin(), anchor.end(), bytes(kAnchor).begin(),
                           bytes(kAnchor).end()),
                "acknowledged anchor changed after crash");
        Outcome outcome = Outcome::Old;
        switch (scenario) {
        case Scenario::Create:
            if (exists(pool, "target.bin")) {
                require(pool.Stat("target.bin").size == 0,
                        "created file has unexpected length");
                outcome = Outcome::New;
            }
            break;
        case Scenario::Write: {
            const auto actual = read_all(pool, "target.bin");
            const auto expected = write_payload();
            if (actual == expected) outcome = Outcome::New;
            else require(actual.empty(), "write returned neither old nor new content");
            break;
        }
        case Scenario::Overwrite: {
            const auto actual = read_all(pool, "target.bin");
            std::vector<std::byte> old(8192, std::byte{'A'});
            auto expected = old;
            expected[4095] = std::byte{'B'};
            expected[4096] = std::byte{'B'};
            expected[4097] = std::byte{'B'};
            if (actual == expected) outcome = Outcome::New;
            else require(actual == old, "overwrite returned mixed or wrong content");
            break;
        }
        case Scenario::Replace: {
            const auto target = pool.Stat("target.bin");
            const auto actual = read_all(pool, "target.bin");
            if (exists(pool, "source.bin")) {
                require(pool.Stat("source.bin").id == 3 && target.id == 4 &&
                            read_all(pool, "source.bin") ==
                                std::vector<std::byte>(8192, std::byte{'S'}) &&
                            actual == std::vector<std::byte>(8192, std::byte{'T'}),
                        "replace returned neither complete old nor new state");
            } else {
                require(target.id == 3 &&
                            actual == std::vector<std::byte>(8192, std::byte{'S'}),
                        "replace lost source identity or returned wrong content");
                outcome = Outcome::New;
            }
            break;
        }
        case Scenario::Delete:
            if (!exists(pool, "target.bin")) outcome = Outcome::New;
            else {
                const auto actual = read_all(pool, "target.bin");
                require(actual == std::vector<std::byte>(8192, std::byte{'D'}),
                        "deleted file has wrong retained content");
            }
            break;
        case Scenario::Rotation:
            if (!exists(pool, "rotating.bin")) outcome = Outcome::New;
            else require(pool.Stat("rotating.bin").size == 0,
                         "rotation target has unexpected length");
            break;
        }
        if (pool.RecoveredIncompleteTail()) {
            bool read_only = false;
            try { pool.Create("probe-after-crash.bin"); }
            catch (const poolfs::Error& error) {
                read_only = error.code() == poolfs::ErrorCode::ReadOnly;
            }
            require(read_only, "incomplete tail was not read-only");
        }
        if (require_new) require(outcome == Outcome::New,
                                 "acknowledged child operation was lost");
        return outcome;
    } catch (const poolfs::Error& error) {
        if (!require_new && error.code() == poolfs::ErrorCode::Corrupt)
            return Outcome::FailClosed;
        throw;
    }
}

std::wstring quote(const std::wstring& value) { return L"\"" + value + L"\""; }

DWORD run_child(Scenario scenario, const std::array<fs::path, 2>& pair,
                const std::wstring& fault) {
    wchar_t program[32768]{};
    const auto length = GetModuleFileNameW(nullptr, program,
        static_cast<DWORD>(std::size(program)));
    require(length && length < std::size(program), "cannot locate crash test executable");
    const auto name = scenario_name(scenario);
    std::wstring command = quote(program) + L" --child " +
        std::wstring(name.begin(), name.end()) +
        L" " + quote(pair[0].wstring()) + L" " + quote(pair[1].wstring());
    require(SetEnvironmentVariableW(L"POOLFS_TEST_V4_CRASH_AT", fault.c_str()) != 0,
            "cannot set child fault environment");
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    PROCESS_INFORMATION process{};
    const BOOL started = CreateProcessW(nullptr, command.data(), nullptr, nullptr,
        FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &startup, &process);
    SetEnvironmentVariableW(L"POOLFS_TEST_V4_CRASH_AT", nullptr);
    require(started != 0, "cannot launch crash test child");
    const auto wait = WaitForSingleObject(process.hProcess, kChildTimeoutMs);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(process.hProcess, 87);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
        throw std::runtime_error("crash test child timed out");
    }
    DWORD code = 0;
    const auto got_code = GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    require(got_code != 0, "cannot read crash test child exit code");
    return code;
}

void remove_pair(const std::array<fs::path, 2>& pair) {
    fs::remove(pair[0]);
    fs::remove(pair[1]);
}

struct Counts {
    int boundaries = 0;
    int old = 0;
    int newer = 0;
    int fail_closed = 0;
};

void record(Counts& counts, Outcome outcome) {
    switch (outcome) {
    case Outcome::Old: ++counts.old; break;
    case Outcome::New: ++counts.newer; break;
    case Outcome::FailClosed: ++counts.fail_closed; break;
    }
}

Counts sweep(Scenario scenario, const fs::path& dir) {
    const auto name = scenario_name(scenario);
    const std::array<fs::path, 2> source{dir / (name + "-template-a.img"),
                                         dir / (name + "-template-b.img")};
    const std::array<fs::path, 2> trial{dir / (name + "-trial-a.img"),
                                        dir / (name + "-trial-b.img")};
    seed(scenario, source);
    Counts counts;
    for (const auto mode : {L"before", L"after"}) {
        const auto limit = std::wstring_view(mode) == L"before"
            ? kMaximumIoPoints : counts.boundaries;
        for (int point = 1; point <= limit; ++point) {
            fs::copy_file(source[0], trial[0]);
            fs::copy_file(source[1], trial[1]);
            const auto fault = std::wstring(mode) + L":" + std::to_wstring(point);
            const auto exit_code = run_child(scenario, trial, fault);
            require(exit_code == 0 || exit_code == kInjectedCrashExit,
                    name + " child failed with code " + std::to_string(exit_code) +
                    " at " + std::string(fault.begin(), fault.end()));
            const auto outcome = verify(scenario, trial, exit_code == 0);
            if (std::wstring_view(mode) == L"before" && point == 1)
                require(outcome == Outcome::Old,
                        name + " changed before its first v4 I/O boundary");
            if (std::wstring_view(mode) == L"after" && point == counts.boundaries)
                require(outcome == Outcome::New,
                        name + " lost its fully flushed final journal record");
            if (exit_code == kInjectedCrashExit) record(counts, outcome);
            remove_pair(trial);
            if (exit_code == 0) {
                require(std::wstring_view(mode) == L"before",
                        name + " after sweep had too few I/O events");
                counts.boundaries = point - 1;
                break;
            }
        }
    }
    remove_pair(source);
    require(counts.boundaries >= 6, name + " exercised too few I/O boundaries");
    if (scenario == Scenario::Replace)
        require(counts.fail_closed == 0, "replace recovery failed closed");
    return counts;
}

void cleanup(const fs::path& dir) {
    for (const auto scenario : {Scenario::Create, Scenario::Write,
                                Scenario::Overwrite, Scenario::Replace,
                                Scenario::Delete, Scenario::Rotation}) {
        const auto name = scenario_name(scenario);
        for (const auto suffix : {"-template-a.img", "-template-b.img",
                                  "-trial-a.img", "-trial-b.img"})
            fs::remove(dir / (name + suffix));
    }
    fs::remove(dir); // Never recursively remove an unexpected file.
}
} // namespace

int main(int argc, char** argv) {
    try {
        // Test fixture for poolfs-check: on three fresh members, after:6
        // stops after the second member's final journal sync. Two copies
        // contain the uncertain record and one retains the clean end marker.
        if (argc >= 5 && argc <= 18 &&
            (std::string_view(argv[1]) == "--create-on-members" ||
             std::string_view(argv[1]) == "--create-different-on-members")) {
            std::vector<fs::path> paths;
            for (int i = 2; i < argc; ++i) paths.emplace_back(argv[i]);
            CrashPool pool(paths);
            pool.Create(std::string_view(argv[1]) == "--create-on-members" ?
                        "uncertain.bin" : "different.bin");
            return 0;
        }
        if (argc >= 5 && argc <= 18 &&
            std::string_view(argv[1]) == "--write-after-repair") {
            std::vector<fs::path> paths;
            for (int i = 2; i < argc; ++i) paths.emplace_back(argv[i]);
            CrashPool pool(paths);
            require(!pool.RecoveredIncompleteTail(),
                    "repaired pool still has an incomplete journal tail");
            require(!exists(pool, "uncertain.bin"),
                    "uncertain final operation survived repair");
            pool.Create("repaired.bin");
            const auto payload = write_payload();
            require(pool.Write("repaired.bin", 0, payload) == payload.size(),
                    "repaired pool did not accept a full write");
            require(read_all(pool, "repaired.bin") == payload,
                    "repaired pool returned wrong file bytes");
            return 0;
        }
        if (argc == 5 && std::string_view(argv[1]) == "--child") {
            run_operation(parse_scenario(argv[2]), {fs::path(argv[3]), fs::path(argv[4])});
            return 0;
        }
        std::vector<Scenario> selected;
        if (argc == 3 && std::string_view(argv[1]) == "--scenario") {
            const auto scenario = parse_scenario(argv[2]);
            require(std::find(kScenarios.begin(), kScenarios.end(), scenario) !=
                        kScenarios.end(),
                    "scenario is not available for this format");
            selected.push_back(scenario);
        } else {
            require(argc == 1,
                    "usage: poolfs-crash-tests.exe [--scenario name]");
            selected.assign(kScenarios.begin(), kScenarios.end());
        }
        require(std::getenv(kFaultEnv.data()) == nullptr,
                "unset POOLFS_TEST_V4_CRASH_AT before running sweep");
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        const auto dir = fs::temp_directory_path() /
            ("poolfs-" + std::string(kFormatName) + "-crash-tests-" + std::to_string(stamp));
        fs::create_directories(dir);
        try {
            for (const auto scenario : selected) {
                const auto start = std::chrono::steady_clock::now();
                const auto counts = sweep(scenario, dir);
                const auto seconds = std::chrono::duration<double>(
                    std::chrono::steady_clock::now() - start).count();
                std::cout << scenario_name(scenario) << ": " << counts.boundaries
                          << " boundaries, " << counts.old << " old, " << counts.newer
                          << " new, " << counts.fail_closed << " fail-closed, "
                          << seconds << " s\n" << std::flush;
            }
        } catch (...) {
            cleanup(dir);
            throw;
        }
        cleanup(dir);
        std::cout << "poolfs " << kFormatName << " crash sweep passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "poolfs " << kFormatName << " crash sweep failed: " << error.what() << '\n';
        return 1;
    }
}
