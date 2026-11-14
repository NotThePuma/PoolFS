#include "gui_partition_ops.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include "gui_partition_script_hash.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
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

struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    explicit Handle(HANDLE initial = INVALID_HANDLE_VALUE) : value(initial) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    void reset(HANDLE next = INVALID_HANDLE_VALUE) {
        if (value && value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};

[[noreturn]] void windows_error(const char* action) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), action);
}

std::wstring action_name(PartitionAction action) {
    switch (action) {
    case PartitionAction::InitializeGpt: return L"InitializeGpt";
    case PartitionAction::CreateAndFormat: return L"CreateAndFormat";
    case PartitionAction::Format: return L"Format";
    case PartitionAction::Remove: return L"Remove";
    case PartitionAction::ChangeLetter: return L"ChangeLetter";
    }
    throw std::invalid_argument("unknown partition operation");
}

bool valid_letter(wchar_t letter) {
    return (letter >= L'A' && letter <= L'Z') ||
           (letter >= L'a' && letter <= L'z');
}

bool valid_filesystem(const std::wstring& name) {
    return name == L"NTFS" || name == L"exFAT" || name == L"FAT32";
}

void validate(const PartitionRequest& request, bool apply) {
    (void)action_name(request.action);
    if (!request.expected_disk_bytes)
        throw std::invalid_argument("partition action needs a disk size snapshot");
    if (request.action == PartitionAction::InitializeGpt) {
        if (!request.expected_disk_guid.empty())
            throw std::invalid_argument("RAW disk initialization must not have a GPT GUID");
    } else if (request.expected_disk_guid.empty()) {
        throw std::invalid_argument("partition action needs the target GPT disk GUID");
    }
    if (request.action == PartitionAction::Format ||
        request.action == PartitionAction::Remove ||
        request.action == PartitionAction::ChangeLetter) {
        if (!request.partition_number || request.expected_partition_guid.empty() ||
            !request.expected_partition_bytes)
            throw std::invalid_argument("partition action needs the target partition identity");
    }
    if ((request.action == PartitionAction::CreateAndFormat ||
         request.action == PartitionAction::ChangeLetter) &&
        !valid_letter(request.drive_letter))
        throw std::invalid_argument("select an unused A-Z drive letter");
    if ((request.action == PartitionAction::CreateAndFormat ||
         request.action == PartitionAction::Format) &&
        !valid_filesystem(request.filesystem))
        throw std::invalid_argument("file system must be NTFS, exFAT, or FAT32");
    if (request.action == PartitionAction::CreateAndFormat &&
        request.size_mib > std::numeric_limits<std::uint64_t>::max() /
            (1024ULL * 1024))
        throw std::invalid_argument("partition size is too large");
    if (request.label.size() > 32)
        throw std::invalid_argument("volume label is too long");
    for (const auto c : request.label)
        if (c < 0x20 || std::wstring_view(L"\\/:*?\"<>|").find(c) !=
                            std::wstring_view::npos)
            throw std::invalid_argument("volume label contains an invalid character");
    if (apply && request.disk_token.empty())
        throw std::invalid_argument("partition action needs a fresh preview token");
    if (apply && !partition_operations_elevated())
        throw std::runtime_error("run PoolFS Manager as Administrator to change partitions");
}

std::wstring quote(std::wstring_view input) {
    std::wstring output = L"\"";
    std::size_t slashes = 0;
    for (const auto c : input) {
        if (c == L'\\') { ++slashes; continue; }
        output.append(slashes * (c == L'"' ? 2 : 1), L'\\');
        slashes = 0;
        if (c == L'"') output.push_back(L'\\');
        output.push_back(c);
    }
    output.append(slashes * 2, L'\\');
    output.push_back(L'"');
    return output;
}

std::wstring command_line(const fs::path& executable,
                          const std::vector<std::wstring>& arguments) {
    std::wstring result = quote(executable.wstring());
    for (const auto& argument : arguments) result += L" " + quote(argument);
    if (result.size() >= 32767)
        throw std::runtime_error("partition helper command line exceeds Windows limit");
    return result;
}

fs::path system_powershell() {
    std::array<wchar_t, 32768> folder{};
    const UINT length = GetSystemDirectoryW(folder.data(),
                                           static_cast<UINT>(folder.size()));
    if (!length || length >= folder.size())
        throw std::runtime_error("cannot locate Windows system directory");
    const auto program = fs::path(folder.data()) /
        L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    if (!fs::is_regular_file(program))
        throw std::runtime_error("Windows PowerShell is unavailable");
    return program;
}

fs::path sibling_script() {
    static int module_anchor;
    HMODULE module = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
            GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            reinterpret_cast<LPCWSTR>(&module_anchor), &module))
        windows_error("cannot locate PoolFS partition module");
    std::array<wchar_t, 32768> path{};
    const DWORD length = GetModuleFileNameW(module, path.data(),
                                           static_cast<DWORD>(path.size()));
    if (!length || length >= path.size())
        throw std::runtime_error("cannot locate PoolFS partition module path");
    const auto script = fs::path(path.data(), path.data() + length).parent_path() /
        L"poolfs-partitions.ps1";
    if (!fs::is_regular_file(script))
        throw std::runtime_error("poolfs-partitions.ps1 is missing beside the backend module");
    return script;
}

void verify_script(HANDLE file) {
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size)) windows_error("cannot size partition helper");
    if (size.QuadPart <= 0 || size.QuadPart > 1024 * 1024)
        throw std::runtime_error("partition helper has an invalid size");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(size.QuadPart));
    std::size_t read = 0;
    while (read < bytes.size()) {
        DWORD received = 0;
        if (!ReadFile(file, bytes.data() + read,
                static_cast<DWORD>(bytes.size() - read), &received, nullptr))
            windows_error("cannot read partition helper");
        if (!received)
            throw std::runtime_error("partition helper changed while reading");
        read += received;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
            nullptr, 0) < 0)
        throw std::runtime_error("cannot open SHA-256 for partition helper");
    std::array<unsigned char, 32> digest{};
    const auto status = BCryptHash(algorithm, nullptr, 0, bytes.data(),
        static_cast<ULONG>(bytes.size()), digest.data(),
        static_cast<ULONG>(digest.size()));
    BCryptCloseAlgorithmProvider(algorithm, 0);
    if (status < 0)
        throw std::runtime_error("cannot hash partition helper");
    constexpr char hex[] = "0123456789abcdef";
    std::string actual;
    actual.reserve(digest.size() * 2);
    for (const auto byte : digest) {
        actual.push_back(hex[byte >> 4]);
        actual.push_back(hex[byte & 0x0f]);
    }
    if (actual != POOLFS_PARTITION_SCRIPT_SHA256)
        throw std::runtime_error(
            "partition helper differs from this PoolFS Manager build; restore the matching file");
}

class HandleList {
public:
    explicit HandleList(std::array<HANDLE, 2> handles) : handles_(handles) {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &bytes);
        if (!bytes) windows_error("cannot size partition helper handle list");
        storage_.resize(bytes);
        list_ = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage_.data());
        if (!InitializeProcThreadAttributeList(list_, 1, 0, &bytes))
            windows_error("cannot prepare partition helper handle list");
        initialized_ = true;
        if (!UpdateProcThreadAttribute(list_, 0,
                PROC_THREAD_ATTRIBUTE_HANDLE_LIST, handles_.data(),
                sizeof(handles_), nullptr, nullptr))
            windows_error("cannot restrict partition helper handles");
    }
    ~HandleList() { if (initialized_) DeleteProcThreadAttributeList(list_); }
    HandleList(const HandleList&) = delete;
    HandleList& operator=(const HandleList&) = delete;
    LPPROC_THREAD_ATTRIBUTE_LIST get() const { return list_; }
private:
    std::array<HANDLE, 2> handles_{};
    std::vector<std::byte> storage_;
    LPPROC_THREAD_ATTRIBUTE_LIST list_ = nullptr;
    bool initialized_ = false;
};

std::wstring decode_output(std::string_view bytes) {
    if (bytes.empty()) return {};
    if (bytes.size() > static_cast<std::size_t>(std::numeric_limits<int>::max()))
        throw std::runtime_error("partition helper output is too large");
    const int input_length = static_cast<int>(bytes.size());
    UINT code_page = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int length = MultiByteToWideChar(code_page, flags, bytes.data(),
                                    input_length, nullptr, 0);
    if (!length) {
        code_page = CP_ACP;
        flags = 0;
        length = MultiByteToWideChar(code_page, flags, bytes.data(),
                                     input_length, nullptr, 0);
    }
    if (!length) return L"Partition helper returned unreadable output";
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    if (MultiByteToWideChar(code_page, flags, bytes.data(), input_length,
                            text.data(), length) != length)
        return L"Partition helper returned unreadable output";
    return text;
}

std::string encode_error(std::wstring_view message) {
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        message.data(), static_cast<int>(message.size()), nullptr, 0,
        nullptr, nullptr);
    if (!bytes) return "partition helper failed";
    std::string output(static_cast<std::size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, message.data(),
        static_cast<int>(message.size()), output.data(), bytes, nullptr, nullptr);
    return output;
}

struct ProcessResult {
    DWORD code = 0;
    std::wstring output;
    bool truncated = false;
};

ProcessResult run_helper(const std::vector<std::wstring>& arguments,
                         bool apply) {
    const auto program = system_powershell();
    const auto script = sibling_script();
    // Keep this read handle open with no write/delete sharing until the
    // PowerShell process exits, so the verified script cannot be swapped.
    Handle script_lock(CreateFileW(script.c_str(), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr));
    if (script_lock.value == INVALID_HANDLE_VALUE)
        windows_error("cannot lock partition helper for reading");
    verify_script(script_lock.value);
    std::vector<std::wstring> command_args{
        L"-NoLogo", L"-NoProfile", L"-NonInteractive", L"-File",
        script.wstring()};
    command_args.insert(command_args.end(), arguments.begin(), arguments.end());
    auto command = command_line(program, command_args);

    SECURITY_ATTRIBUTES attributes{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    Handle read_pipe, write_pipe;
    if (!CreatePipe(&read_pipe.value, &write_pipe.value, &attributes, 0) ||
        !SetHandleInformation(read_pipe.value, HANDLE_FLAG_INHERIT, 0))
        windows_error("cannot create partition helper output pipe");
    Handle null_input(CreateFileW(L"NUL", GENERIC_READ,
        FILE_SHARE_READ | FILE_SHARE_WRITE, &attributes, OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL, nullptr));
    if (null_input.value == INVALID_HANDLE_VALUE)
        windows_error("cannot prepare partition helper input");
    HandleList allowed({write_pipe.value, null_input.value});
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = null_input.value;
    startup.StartupInfo.hStdOutput = write_pipe.value;
    startup.StartupInfo.hStdError = write_pipe.value;
    startup.lpAttributeList = allowed.get();
    PROCESS_INFORMATION info{};
    if (!CreateProcessW(program.c_str(), command.data(), nullptr, nullptr,
            TRUE, CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
            nullptr, &startup.StartupInfo, &info))
        windows_error("cannot start partition helper");
    Handle child(info.hProcess), thread(info.hThread);
    write_pipe.reset();
    null_input.reset();

    std::string output;
    bool truncated = false;
    const auto preview_deadline = std::chrono::steady_clock::now() +
        std::chrono::seconds(60);
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr,
                &available, nullptr)) {
            const auto error = GetLastError();
            if (error == ERROR_BROKEN_PIPE) break;
            if (apply) WaitForSingleObject(child.value, INFINITE);
            SetLastError(error);
            windows_error("cannot inspect partition helper output");
        }
        if (available) {
            std::array<char, 4096> buffer{};
            DWORD received = 0;
            if (!ReadFile(read_pipe.value, buffer.data(),
                    std::min<DWORD>(available,
                                    static_cast<DWORD>(buffer.size())),
                    &received, nullptr)) {
                const auto error = GetLastError();
                if (apply) WaitForSingleObject(child.value, INFINITE);
                SetLastError(error);
                windows_error("cannot read partition helper output");
            }
            if (output.size() + received > 1024 * 1024) {
                if (!apply) {
                    TerminateProcess(child.value, 1);
                    WaitForSingleObject(child.value, 1000);
                    throw std::runtime_error("partition preview output exceeds 1 MiB");
                }
                // Continue draining and wait for an applying process. Exiting
                // here could leave a disk mutation running without a result.
                truncated = true;
                continue;
            }
            output.append(buffer.data(), received);
            continue;
        }
        if (WaitForSingleObject(child.value, 0) == WAIT_OBJECT_0) {
            DWORD remaining = 0;
            if (!PeekNamedPipe(read_pipe.value, nullptr, 0, nullptr,
                    &remaining, nullptr) || remaining == 0)
                break;
            continue;
        }
        if (!apply && std::chrono::steady_clock::now() >= preview_deadline) {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 1000);
            throw std::runtime_error("partition preview did not finish within 60 seconds");
        }
        Sleep(50);
    }
    const DWORD wait = WaitForSingleObject(child.value, apply ? INFINITE : 1000);
    if (wait != WAIT_OBJECT_0) {
        if (!apply) {
            TerminateProcess(child.value, 1);
            WaitForSingleObject(child.value, 1000);
        }
        throw std::runtime_error("partition helper did not finish; inspect disk state before retrying");
    }
    DWORD code = 0;
    if (!GetExitCodeProcess(child.value, &code))
        windows_error("cannot read partition helper exit status");
    return {code, decode_output(output), truncated};
}

std::wstring token_from_output(std::wstring& output) {
    constexpr std::wstring_view prefix = L"POOLFS_DISK_TOKEN=";
    std::wstring token;
    std::size_t search = 0;
    for (;;) {
        const auto position = output.find(prefix, search);
        if (position == std::wstring::npos) break;
        if (position && output[position - 1] != L'\n') {
            search = position + prefix.size();
            continue;
        }
        const auto start = position + prefix.size();
        const auto end = output.find_first_of(L"\r\n", start);
        const auto candidate = output.substr(start, end - start);
        const auto erase_end = end == std::wstring::npos ? output.size() :
            output.find_first_not_of(L"\r\n", end);
        output.erase(position, erase_end == std::wstring::npos ?
            std::wstring::npos : erase_end - position);
        if (token.empty() && !candidate.empty() && candidate.size() <= 4096 &&
            candidate.find_first_not_of(
                L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/=") ==
                std::wstring::npos)
            token = candidate;
        search = position;
    }
    return token;
}

} // namespace

bool partition_operations_elevated() {
    Handle token;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &token.value))
        return false;
    TOKEN_ELEVATION elevation{};
    DWORD bytes = 0;
    if (!GetTokenInformation(token.value, TokenElevation, &elevation,
            sizeof(elevation), &bytes)) return false;
    return elevation.TokenIsElevated != 0;
}

PartitionResult run_partition_operation(const PartitionRequest& request,
                                        bool apply) {
    validate(request, apply);
    std::vector<std::wstring> arguments{
        L"-Action", action_name(request.action),
        L"-DiskNumber", std::to_wstring(request.disk_number),
        L"-ExpectedDiskBytes", std::to_wstring(request.expected_disk_bytes),
        L"-ExpectedDiskGuid", request.expected_disk_guid,
        L"-ExpectedPartitionNumber", std::to_wstring(request.partition_number),
        L"-ExpectedPartitionGuid", request.expected_partition_guid,
        L"-ExpectedPartitionOffset", std::to_wstring(request.expected_partition_offset),
        L"-ExpectedPartitionBytes", std::to_wstring(request.expected_partition_bytes),
        L"-SizeMiB", std::to_wstring(request.size_mib),
        L"-DriveLetter", request.drive_letter ?
            std::wstring(1, request.drive_letter) : L"",
        L"-FileSystem", request.filesystem,
        L"-Label", request.label,
        L"-ExpectedDiskToken", request.disk_token};
    if (apply) arguments.push_back(L"-Apply");
    auto outcome = run_helper(arguments, apply);
    if (outcome.truncated)
        outcome.output += L"\r\nOutput was truncated after 1 MiB.";
    auto token = token_from_output(outcome.output);
    if (outcome.code != 0)
        throw std::runtime_error("partition helper failed: " +
            encode_error(outcome.output));
    if (token.empty())
        throw std::runtime_error("partition helper did not return a disk identity token");
    while (!outcome.output.empty() &&
           (outcome.output.back() == L'\r' || outcome.output.back() == L'\n'))
        outcome.output.pop_back();
    if (outcome.output.empty())
        outcome.output = apply ? L"Partition operation completed." :
                                 L"Partition operation is ready for review.";
    return {std::move(outcome.output), std::move(token)};
}

} // namespace poolfs::gui
