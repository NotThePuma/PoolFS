#include "gui_backend.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cwchar>
#include <filesystem>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace poolfs::gui {
namespace {

namespace fs = std::filesystem;

struct UniqueHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit UniqueHandle(HANDLE handle = INVALID_HANDLE_VALUE) : value(handle) {}
    ~UniqueHandle() { reset(); }
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    void reset(HANDLE next = INVALID_HANDLE_VALUE) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};

[[noreturn]] void windows_error(const char* action) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), action);
}

wchar_t checked_drive(wchar_t drive) {
    if (drive >= L'a' && drive <= L'z') drive -= L'a' - L'A';
    if (drive < L'A' || drive > L'Z')
        throw std::invalid_argument("drive must be an A-Z letter");
    return drive;
}

std::wstring event_name(wchar_t drive) {
    return std::wstring(L"Local\\PoolFS.Mount.") + checked_drive(drive);
}

std::wstring drive_root(wchar_t drive) {
    return std::wstring(1, checked_drive(drive)) + L":\\";
}

fs::path sibling_executable(std::wstring_view name) {
    static int module_anchor;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_anchor), &module))
        windows_error("cannot locate PoolFS backend module");
    std::array<wchar_t, 32768> buffer{};
    const DWORD length = GetModuleFileNameW(module, buffer.data(),
                                           static_cast<DWORD>(buffer.size()));
    if (!length || length >= buffer.size())
        throw std::runtime_error("cannot locate PoolFS backend module path");
    const auto file = fs::path(buffer.data(), buffer.data() + length)
        .parent_path() / fs::path(name);
    if (!fs::is_regular_file(file))
        throw std::runtime_error("required PoolFS tool is missing beside the backend module");
    return file;
}

std::wstring quote(std::wstring_view input) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const auto c : input) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(slashes * (c == L'\"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'\"') output.push_back(L'\\');
        output.push_back(c);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'\"');
    return output;
}

std::wstring command_line(const fs::path& executable,
                          const std::vector<std::wstring>& arguments) {
    std::wstring result = quote(executable.wstring());
    for (const auto& argument : arguments) result += L" " + quote(argument);
    if (result.size() >= 32767)
        throw std::runtime_error(
            "PoolFS command line exceeds Windows' 32767-character limit; shorten member paths");
    return result;
}

std::string utf8(std::wstring_view input) {
    if (input.empty()) return {};
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("text is too long for UTF-8 conversion");
    const int required = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0, nullptr, nullptr);
    if (!required) throw std::runtime_error("invalid Unicode text");
    std::string output(static_cast<std::size_t>(required), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, input.data(),
            static_cast<int>(input.size()), output.data(), required,
            nullptr, nullptr) != required)
        throw std::runtime_error("UTF-8 conversion failed");
    return output;
}

std::wstring wide_text(std::string_view input) {
    if (input.empty()) return {};
    if (input.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("tool output is too long");
    int required = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS,
        input.data(), static_cast<int>(input.size()), nullptr, 0);
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (!required) {
        code_page = CP_ACP;
        flags = 0;
        required = MultiByteToWideChar(code_page, flags, input.data(),
            static_cast<int>(input.size()), nullptr, 0);
    }
    if (!required) return L"PoolFS tool returned unreadable text";
    std::wstring output(static_cast<std::size_t>(required), L'\0');
    if (MultiByteToWideChar(code_page, flags, input.data(),
            static_cast<int>(input.size()), output.data(), required) != required)
        return L"PoolFS tool returned unreadable text";
    return output;
}

void append_json_string(std::string& output, std::wstring_view value) {
    std::wstring escaped;
    escaped.reserve(value.size() + 2);
    escaped.push_back(L'"');
    constexpr wchar_t hex[] = L"0123456789abcdef";
    for (const auto c : value) {
        if (c == L'"' || c == L'\\') {
            escaped.push_back(L'\\');
            escaped.push_back(c);
        } else if (c < 0x20) {
            escaped += L"\\u00";
            escaped.push_back(hex[(c >> 4) & 15]);
            escaped.push_back(hex[c & 15]);
        } else {
            escaped.push_back(c);
        }
    }
    escaped.push_back(L'"');
    output += utf8(escaped);
}

std::string config_json(const MountConfig& config, const fs::path& destination) {
    const auto drive = checked_drive(config.drive);
    validate_pool_name(config.name);
    if (config.members.empty() || config.members.size() > 16)
        throw std::invalid_argument("mount config needs 1 to 16 members");
    const auto base = fs::absolute(destination).lexically_normal().parent_path();
    std::string output = "{\n  \"schema_version\": 1,\n  \"name\": ";
    append_json_string(output, config.name);
    output += ",\n  \"drive\": \"";
    output.push_back(static_cast<char>(drive));
    output += ":\",\n  \"require_encrypted\": ";
    output += config.require_encrypted ? "true" : "false";
    output += ",\n  \"members\": [\n";
    for (std::size_t i = 0; i < config.members.size(); ++i) {
        const auto absolute = (config.members[i].is_absolute() ?
            config.members[i] : base / config.members[i]).lexically_normal();
        const auto relative = absolute.lexically_relative(base);
        const auto stored = !relative.empty() && !relative.has_root_path() ?
            relative : absolute;
        output += "    ";
        append_json_string(output, stored.wstring());
        output += i + 1 == config.members.size() ? "\n" : ",\n";
    }
    output += "  ]\n}\n";
    if (output.size() > 1024 * 1024)
        throw std::invalid_argument("mount config exceeds 1 MiB limit");
    return output;
}

fs::path normalized_target(const fs::path& path) {
    std::error_code error;
    const auto canonical = fs::weakly_canonical(path, error);
    auto normalized = error ? fs::absolute(path).lexically_normal() : canonical;
    normalized.make_preferred();
    return normalized;
}

bool aliases_member(const fs::path& target, const fs::path& member) {
    const auto first = normalized_target(target).wstring();
    const auto second = normalized_target(member).wstring();
    if (first.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
        second.size() <= static_cast<std::size_t>(std::numeric_limits<int>::max()) &&
        CompareStringOrdinal(first.c_str(), static_cast<int>(first.size()),
            second.c_str(), static_cast<int>(second.size()), TRUE) == CSTR_EQUAL)
        return true;
    std::error_code error;
    const bool same_file = fs::equivalent(target, member, error);
    return !error && same_file;
}

class HandleList {
public:
    explicit HandleList(std::vector<HANDLE> handles)
        : handles_(std::move(handles)) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) windows_error("cannot size process handle allowlist");
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes))
            windows_error("cannot initialize process handle allowlist");
        initialized_ = true;
        if (!UpdateProcThreadAttribute(list_, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                handles_.data(), handles_.size() * sizeof(HANDLE), nullptr, nullptr))
            windows_error("cannot set process handle allowlist");
    }
    ~HandleList() { if (initialized_) DeleteProcThreadAttributeList(list_); }
    HandleList(const HandleList&) = delete;
    HandleList& operator=(const HandleList&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }
private:
    std::vector<HANDLE> handles_;
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool initialized_ = false;
};

ToolResult run_capture(const fs::path& executable,
                       const std::vector<std::wstring>& arguments) {
    SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    UniqueHandle read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe.value, &write_pipe.value, &inherit, 0) ||
        !SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0))
        windows_error("cannot create PoolFS output pipe");
    UniqueHandle null_input(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (null_input.value == INVALID_HANDLE_VALUE)
        windows_error("cannot prepare PoolFS tool input");
    HandleList allowed({write_pipe.value, null_input.value});
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.value;
    startup.StartupInfo.hStdOutput = write_pipe.value;
    startup.StartupInfo.hStdError = write_pipe.value;
    startup.lpAttributeList = allowed.get();
    auto command = command_line(executable, arguments);
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
            nullptr, nullptr, &startup.StartupInfo, &info))
        windows_error("cannot start PoolFS tool");
    UniqueHandle child(info.hProcess), thread(info.hThread);
    write_pipe.reset();
    null_input.reset();

    std::string bytes;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(45);
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr,
                &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) break;
            SetLastError(error);
            windows_error("cannot read PoolFS tool output");
        }
        if (available) {
            std::array<char, 4096> buffer{};
            DWORD received = 0;
            if (!ReadFile(read_pipe.value, buffer.data(),
                    std::min<DWORD>(available, static_cast<DWORD>(buffer.size())),
                    &received, nullptr))
                windows_error("cannot receive PoolFS tool output");
            if (bytes.size() + received > 1024 * 1024) {
                TerminateProcess(child.value, 1);
                WaitForSingleObject(child.value, 1000);
                throw std::runtime_error("PoolFS tool output exceeds 1 MiB");
            }
            bytes.append(buffer.data(), received);
            continue;
        }
        if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0) {
            // The process may have written after the preceding peek. Once it
            // has exited, a final peek drains everything it buffered.
            DWORD remaining = 0;
            if (!PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr,
                    &remaining, nullptr) || remaining == 0)
                break;
            continue;
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 1000);
            throw std::runtime_error("PoolFS tool did not finish within 45 seconds");
        }
        Sleep(25);
    }
    const DWORD wait = WaitForSingleObject(child.value, 1000);
    if (wait != WAIT_OBJECT_0) {
        TerminateProcess(child.value, 1);
        throw std::runtime_error("PoolFS tool did not exit after closing output");
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(child.value, &code))
        windows_error("cannot read PoolFS tool exit code");
    return {static_cast<unsigned>(code), wide_text(bytes)};
}

fs::path winfsp_runtime_directory() {
    std::array<wchar_t, 32768> install_dir{};
    DWORD bytes = static_cast<DWORD>(install_dir.size() * sizeof(wchar_t));
    if (RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\WOW6432Node\\WinFsp",
            L"InstallDir", RRF_RT_REG_SZ, nullptr, install_dir.data(),
            &bytes) == ERROR_SUCCESS && install_dir[0])
        return fs::path(install_dir.data()) / L"bin";
    return L"C:\\Program Files (x86)\\WinFsp\\bin";
}

fs::path require_winfsp_runtime() {
    const auto winfsp = winfsp_runtime_directory();
    if (!fs::is_regular_file(winfsp / L"winfsp-x64.dll"))
        throw std::runtime_error(
            "WinFsp is required to mount PoolFS. Install the WinFsp runtime "
            "from https://winfsp.dev/rel/ and try again.");
    return winfsp;
}

std::vector<wchar_t> mount_environment(const fs::path& winfsp) {
    LPWCH block = GetEnvironmentStringsW();
    if (!block) windows_error("cannot read process environment");
    std::vector<std::wstring> entries;
    bool path_seen = false;
    for (const wchar_t* item = block; *item; item += std::wcslen(item) + 1) {
        std::wstring entry(item);
        if (entry.size() >= 5 && entry[4] == L'=' &&
            CompareStringOrdinal(entry.data(), 4, L"Path", 4, TRUE) ==
                CSTR_EQUAL) {
            entry = L"Path=" + winfsp.wstring() + L";" + entry.substr(5);
            path_seen = true;
        }
        entries.push_back(std::move(entry));
    }
    FreeEnvironmentStringsW(block);
    if (!path_seen) entries.push_back(L"Path=" + winfsp.wstring());
    std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) ==
            CSTR_LESS_THAN;
    });
    std::vector<wchar_t> result;
    for (const auto& entry : entries) {
        result.insert(result.end(), entry.begin(), entry.end());
        result.push_back(L'\0');
    }
    result.push_back(L'\0');
    return result;
}

void write_exact(HANDLE file, std::string_view bytes) {
    while (!bytes.empty()) {
        DWORD written = 0;
        const auto portion = static_cast<DWORD>(std::min<std::size_t>(
            bytes.size(), std::numeric_limits<DWORD>::max()));
        if (!WriteFile(file, bytes.data(), portion, &written, nullptr) || !written)
            windows_error("cannot write PoolFS mount config");
        bytes.remove_prefix(written);
    }
}

void validate_secret_for_config(const MountConfig& config,
                                const password::Secret* secret) {
    if (config.members.empty())
        throw std::invalid_argument("mount config has no members");
    const bool encrypted = Pool::V6Encrypted(config.members.front());
    if (config.require_encrypted && !encrypted)
        throw std::runtime_error("this mount config requires an encrypted pool");
    if (encrypted && (!secret || secret->view().empty()))
        throw std::runtime_error("encrypted pool requires its password");
    if (!encrypted && secret)
        throw std::runtime_error("password supplied for an unencrypted pool");
}

void confirm_config_snapshot(const MountConfig& saved,
                             const MountConfig& expected,
                             const fs::path& config_path) {
    if (saved.drive != checked_drive(expected.drive) ||
        saved.name != expected.name ||
        saved.require_encrypted != expected.require_encrypted ||
        saved.members.size() != expected.members.size())
        throw std::runtime_error(
            "mount config changed since it was shown; reload and review it");
    for (std::size_t i = 0; i < saved.members.size(); ++i) {
        const auto shown = expected.members[i].is_absolute() ?
            expected.members[i] :
            config_path.parent_path() / expected.members[i];
        if (!aliases_member(saved.members[i], shown))
            throw std::runtime_error(
                "mount config changed since it was shown; reload and review it");
    }
}

} // namespace

void save_mount_config(const MountConfig& config, const fs::path& destination) {
    if (destination.empty())
        throw std::invalid_argument("mount config destination is empty");
    const auto target = fs::absolute(destination).lexically_normal();
    if (target.filename().empty())
        throw std::invalid_argument("mount config destination needs a file name");
    const auto extension = target.extension().wstring();
    if (CompareStringOrdinal(extension.c_str(),
            static_cast<int>(extension.size()), L".json", 5, TRUE) != CSTR_EQUAL)
        throw std::invalid_argument("mount config must use a .json file name");
    for (const auto& member : config.members) {
        const auto absolute_member = member.is_absolute() ? member :
            target.parent_path() / member;
        if (aliases_member(target, absolute_member))
            throw std::invalid_argument(
                "mount config destination aliases a member image");
    }
    const auto contents = config_json(config, target);
    fs::path temporary;
    UniqueHandle file;
    for (unsigned attempt = 0; attempt < 100; ++attempt) {
        temporary = target;
        temporary += L".poolfs-tmp-" + std::to_wstring(GetCurrentProcessId()) +
            L"-" + std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(attempt);
        file.reset(CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr,
            CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.value != INVALID_HANDLE_VALUE) break;
        if (GetLastError() != ERROR_FILE_EXISTS &&
            GetLastError() != ERROR_ALREADY_EXISTS)
            windows_error("cannot create temporary mount config");
    }
    if (file.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("cannot reserve temporary mount config name");
    try {
        write_exact(file.value, contents);
        if (!FlushFileBuffers(file.value))
            windows_error("cannot flush temporary mount config");
        file.reset();
        (void)load_mount_config(temporary);
        if (!MoveFileExW(temporary.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            windows_error("cannot replace mount config");
    } catch (...) {
        file.reset();
        DeleteFileW(temporary.c_str());
        throw;
    }
}

ToolResult discover_config(const fs::path& config_path) {
    if (config_path.empty())
        throw std::invalid_argument("mount config path is empty");
    return run_capture(sibling_executable(L"poolfsctl.exe"),
        {L"discover-config", fs::absolute(config_path).lexically_normal().wstring()});
}

bool config_is_encrypted(const MountConfig& config) {
    if (config.members.empty())
        throw std::invalid_argument("mount config has no members");
    const bool encrypted = Pool::V6Encrypted(config.members.front());
    if (config.require_encrypted && !encrypted)
        throw std::runtime_error("this mount config requires an encrypted pool");
    return encrypted;
}

bool mounted(wchar_t drive) {
    UniqueHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                  event_name(drive).c_str()));
    if (event.value && event.value != INVALID_HANDLE_VALUE) return true;
    const auto error = GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_INVALID_HANDLE)
        throw std::system_error(static_cast<int>(error), std::system_category(),
                                "cannot inspect PoolFS mount event");
    return false;
}

void mount_config(const fs::path& config_path, const password::Secret* secret) {
    const auto expected = load_mount_config(config_path);
    mount_config(config_path, expected, secret);
}

void mount_config(const fs::path& config_path, const MountConfig& expected,
                  const password::Secret* secret) {
    const auto absolute = fs::absolute(config_path).lexically_normal();
    const auto config = load_mount_config(absolute);
    confirm_config_snapshot(config, expected, absolute);
    const auto winfsp = require_winfsp_runtime();
    const auto discovery = discover_config(absolute);
    if (discovery.exit_code != 0)
        throw std::runtime_error("configured member validation failed: " +
                                 utf8(discovery.output));
    confirm_config_snapshot(load_mount_config(absolute), expected, absolute);
    validate_secret_for_config(config, secret);
    const auto drive = checked_drive(config.drive);
    if (mounted(drive))
        throw std::runtime_error("drive already has a managed PoolFS mount");
    if (GetDriveTypeW(drive_root(drive).c_str()) != DRIVE_NO_ROOT_DIR)
        throw std::runtime_error("drive letter is already occupied");

    const auto executable = sibling_executable(L"poolfs-mount.exe");
    UniqueHandle pipe_read, pipe_write;
    std::unique_ptr<HandleList> allowed;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = secret ? sizeof(startup) : sizeof(STARTUPINFOW);
    DWORD flags = CREATE_NO_WINDOW;
    std::vector<std::wstring> arguments;
    if (secret) {
        SECURITY_ATTRIBUTES inherit{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
        if (!CreatePipe(&pipe_read.value, &pipe_write.value, &inherit, 0) ||
            !SetHandleInformation(pipe_write.value, HANDLE_FLAG_INHERIT, 0))
            windows_error("cannot create mount password pipe");
        allowed = std::make_unique<HandleList>(
            std::vector<HANDLE>{pipe_read.value});
        startup.lpAttributeList = allowed->get();
        flags |= EXTENDED_STARTUPINFO_PRESENT;
        arguments = {L"--password-handle",
            std::to_wstring(reinterpret_cast<std::uintptr_t>(pipe_read.value)),
            L"--named-images-v6", config.name};
    } else {
        arguments = {L"--named-images-v6", config.name};
    }
    arguments.push_back(std::wstring(1, drive) + L":");
    for (const auto& member : config.members)
        arguments.push_back(member.wstring());
    auto command = command_line(executable, arguments);
    auto environment = mount_environment(winfsp);
    if (!environment.empty()) flags |= CREATE_UNICODE_ENVIRONMENT;
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(executable.c_str(), command.data(), nullptr, nullptr,
            secret != nullptr, flags, environment.empty() ? nullptr :
                environment.data(), nullptr, &startup.StartupInfo, &info))
        windows_error("cannot start PoolFS mount process");
    UniqueHandle child(info.hProcess), thread(info.hThread);
    if (secret) {
        pipe_read.reset();
        try {
            password::write_pipe(pipe_write.value, secret->view());
        } catch (...) {
            pipe_write.reset();
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 1000);
            throw;
        }
        pipe_write.reset();
    }
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0) {
            DWORD code = 0;
            if (!GetExitCodeProcess(child.value, &code))
                windows_error("cannot inspect PoolFS mount exit status");
            if (secret && code == 5)
                throw std::runtime_error("invalid PoolFS password or encrypted header");
            if (code == 0xC0000135)
                throw std::runtime_error(
                    "PoolFS mount could not start because a required DLL is missing. "
                    "Check the WinFsp installation and keep the PoolFS package files together.");
            throw std::runtime_error("PoolFS mount exited before becoming ready (exit " +
                                     std::to_string(code) + ")");
        }
        if (mounted(drive) &&
            GetDriveTypeW(drive_root(drive).c_str()) != DRIVE_NO_ROOT_DIR)
            return;
        Sleep(125);
    }
    TerminateProcess(child.value, 1);
    WaitForSingleObject(child.value, 1000);
    throw std::runtime_error("PoolFS mount did not become ready within 10 seconds");
}

void unmount(wchar_t drive) {
    drive = checked_drive(drive);
    UniqueHandle event(OpenEventW(EVENT_MODIFY_STATE, FALSE,
                                  event_name(drive).c_str()));
    if (!event.value || event.value == INVALID_HANDLE_VALUE)
        throw std::runtime_error("no managed PoolFS mount on this drive");
    if (!SetEvent(event.value)) windows_error("cannot request PoolFS unmount");
    event.reset();
    for (int attempt = 0; attempt < 80; ++attempt) {
        if (!mounted(drive)) return;
        Sleep(125);
    }
    throw std::runtime_error("PoolFS unmount did not finish within 10 seconds");
}

ToolResult reclaim(wchar_t drive) {
    drive = checked_drive(drive);
    const auto report = run_capture(sibling_executable(L"poolfsctl.exe"),
        {L"reclaim", std::wstring(1, drive) + L":"});
    if (report.exit_code != 0)
        throw std::runtime_error("PoolFS reclaim failed: " + utf8(report.output));
    return report;
}

void format_v6_images(const std::vector<ImageMemberSpec>& members,
                      const password::Secret* secret) {
    if (secret) {
        if (secret->view().size() < 12)
            throw std::invalid_argument("PoolFS password must be at least 12 UTF-8 bytes");
        Pool::FormatV6EncryptedImages(members, secret->view());
    } else {
        Pool::FormatV6Images(members);
    }
}

VerifyResult verify_pool(const MountConfig& config,
                         const password::Secret* secret) {
    validate_secret_for_config(config, secret);
    std::unique_ptr<Pool> pool;
    if (secret) {
        pool = std::make_unique<Pool>(Pool::V6ReadOnlyImageTag{},
                                      config.members, secret->view());
    } else {
        pool = std::make_unique<Pool>(Pool::V6ReadOnlyImageTag{},
                                      config.members);
    }
    VerifyResult result;
    result.capacity = pool->Capacity();
    result.available = pool->Available();
    result.incomplete_tail = pool->RecoveredIncompleteTail();
    result.issues = pool->Scrub({});
    return result;
}

} // namespace poolfs::gui
