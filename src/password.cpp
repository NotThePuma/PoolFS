#include "password.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace poolfs::password {
namespace {

constexpr std::size_t kMaximumUtf16Units = 1024;
constexpr std::size_t kMaximumUtf8Bytes = 4096;

void wipe(std::string& text) noexcept {
    if (!text.empty()) SecureZeroMemory(text.data(), text.size());
    text.clear();
}

// Small-string moves can copy bytes into the destination while setting the
// source length to zero. Restore its original length only when its existing
// capacity can hold it, then erase the former inline storage as well.
void wipe_moved_source(std::string& text, std::size_t former_size) noexcept {
    if (text.capacity() >= former_size) {
        text.resize(former_size);
        wipe(text);
    }
}

struct ConsoleModeRestore {
    HANDLE input;
    DWORD mode;
    ~ConsoleModeRestore() { SetConsoleMode(input, mode); }
};

struct WideInput {
    std::array<wchar_t, kMaximumUtf16Units + 2> data{};
    ~WideInput() { SecureZeroMemory(data.data(), data.size() * sizeof(wchar_t)); }
};

Secret read_one(const wchar_t* label) {
    const HANDLE input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD original_mode = 0;
    if (input == INVALID_HANDLE_VALUE || !input ||
        !GetConsoleMode(input, &original_mode))
        throw std::runtime_error("password entry requires an interactive console");
    if (!(original_mode & ENABLE_LINE_INPUT))
        throw std::runtime_error("password entry requires console line input");
    if (!SetConsoleMode(input, original_mode & ~ENABLE_ECHO_INPUT))
        throw std::runtime_error("could not disable password echo");
    ConsoleModeRestore restore{input, original_mode};

    std::wcerr << label << std::flush;
    WideInput input_text;
    DWORD read = 0;
    const BOOL ok = ReadConsoleW(input, input_text.data.data(),
        static_cast<DWORD>(input_text.data.size()), &read, nullptr);
    std::wcerr << L'\n' << std::flush;
    if (!ok || !read)
        throw std::runtime_error("password entry was cancelled");
    const auto ended = [&] {
        return input_text.data[read - 1] == L'\r' ||
            input_text.data[read - 1] == L'\n';
    };
    if (!ended()) {
        // ReadConsoleW can return a full buffer before Enter. Consume the
        // rest of that line while echo is still disabled, so trailing input
        // cannot become a shell command after this process exits.
        bool drained = false;
        for (int attempt = 0; attempt < 64 && !drained; ++attempt) {
            DWORD extra = 0;
            SecureZeroMemory(input_text.data.data(),
                input_text.data.size() * sizeof(wchar_t));
            if (!ReadConsoleW(input, input_text.data.data(),
                    static_cast<DWORD>(input_text.data.size()), &extra, nullptr) || !extra)
                break;
            drained = input_text.data[extra - 1] == L'\r' ||
                input_text.data[extra - 1] == L'\n';
        }
        if (!drained) FlushConsoleInputBuffer(input);
        throw std::runtime_error("password length must be 1 to 1024 characters");
    }
    while (read && (input_text.data[read - 1] == L'\r' ||
                    input_text.data[read - 1] == L'\n')) --read;
    if (read == 0 || read > kMaximumUtf16Units)
        throw std::runtime_error("password length must be 1 to 1024 characters");
    const int bytes = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
        input_text.data.data(), static_cast<int>(read), nullptr, 0, nullptr, nullptr);
    if (!bytes || static_cast<std::size_t>(bytes) > kMaximumUtf8Bytes)
        throw std::runtime_error("password contains invalid Unicode or is too long");
    std::string utf8(static_cast<std::size_t>(bytes), '\0');
    if (WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS,
            input_text.data.data(), static_cast<int>(read), utf8.data(), bytes,
            nullptr, nullptr) != bytes) {
        wipe(utf8);
        throw std::runtime_error("password conversion failed");
    }
    return Secret(std::move(utf8));
}

void write_exact(HANDLE pipe, const void* data, std::size_t length) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    while (length) {
        DWORD sent = 0;
        if (!WriteFile(pipe, bytes, static_cast<DWORD>(length), &sent, nullptr) || !sent)
            throw std::runtime_error("could not pass password to mount process");
        bytes += sent;
        length -= sent;
    }
}

void read_exact(HANDLE pipe, void* data, std::size_t length) {
    auto* bytes = static_cast<std::uint8_t*>(data);
    while (length) {
        DWORD received = 0;
        if (!ReadFile(pipe, bytes, static_cast<DWORD>(length), &received, nullptr) || !received)
            throw std::runtime_error("could not receive password from controller");
        bytes += received;
        length -= received;
    }
}

} // namespace

Secret::Secret(std::string&& value) {
    const auto length = value.size();
    value_ = std::move(value);
    wipe_moved_source(value, length);
}

Secret::~Secret() { wipe(value_); }

Secret::Secret(Secret&& other) noexcept {
    const auto length = other.value_.size();
    value_ = std::move(other.value_);
    wipe_moved_source(other.value_, length);
}

Secret& Secret::operator=(Secret&& other) noexcept {
    if (this != &other) {
        wipe(value_);
        const auto length = other.value_.size();
        value_ = std::move(other.value_);
        wipe_moved_source(other.value_, length);
    }
    return *this;
}

Secret prompt(const wchar_t* label, bool confirm) {
    Secret first = read_one(label);
    if (!confirm) return first;
    Secret second = read_one(L"Confirm PoolFS password: ");
    const auto a = first.view(), b = second.view();
    unsigned difference = static_cast<unsigned>(a.size() ^ b.size());
    for (std::size_t i = 0; i < std::max(a.size(), b.size()); ++i) {
        const unsigned left = i < a.size() ? static_cast<unsigned char>(a[i]) : 0;
        const unsigned right = i < b.size() ? static_cast<unsigned char>(b[i]) : 0;
        difference |= left ^ right;
    }
    if (difference) throw std::runtime_error("PoolFS passwords did not match");
    return first;
}

Secret read_pipe(HANDLE pipe) {
    std::array<std::uint8_t, 4> length_bytes{};
    read_exact(pipe, length_bytes.data(), length_bytes.size());
    const std::uint32_t length = std::uint32_t(length_bytes[0]) |
        (std::uint32_t(length_bytes[1]) << 8) |
        (std::uint32_t(length_bytes[2]) << 16) |
        (std::uint32_t(length_bytes[3]) << 24);
    if (length == 0 || length > kMaximumUtf8Bytes)
        throw std::runtime_error("invalid password handoff length");
    std::string value(length, '\0');
    try {
        read_exact(pipe, value.data(), value.size());
    } catch (...) {
        wipe(value);
        throw;
    }
    return Secret(std::move(value));
}

void write_pipe(HANDLE pipe, std::string_view secret) {
    if (secret.empty() || secret.size() > kMaximumUtf8Bytes)
        throw std::runtime_error("invalid password handoff length");
    const auto size = static_cast<std::uint32_t>(secret.size());
    const std::array<std::uint8_t, 4> length_bytes{
        static_cast<std::uint8_t>(size), static_cast<std::uint8_t>(size >> 8),
        static_cast<std::uint8_t>(size >> 16), static_cast<std::uint8_t>(size >> 24)};
    write_exact(pipe, length_bytes.data(), length_bytes.size());
    write_exact(pipe, secret.data(), secret.size());
}

} // namespace poolfs::password
