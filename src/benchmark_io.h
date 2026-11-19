#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <stdexcept>
#include <string>

namespace poolfs::benchmark_io {

class File {
public:
    explicit File(HANDLE h = INVALID_HANDLE_VALUE) : h_(h) {}
    ~File() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    File(File&& other) noexcept : h_(other.h_) { other.h_ = INVALID_HANDLE_VALUE; }
    File& operator=(File&& other) noexcept {
        if (this != &other) {
            if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_);
            h_ = other.h_;
            other.h_ = INVALID_HANDLE_VALUE;
        }
        return *this;
    }
    HANDLE get() const { return h_; }
    void close() {
        if (h_ != INVALID_HANDLE_VALUE) {
            if (!CloseHandle(h_)) throw std::runtime_error("CloseHandle failed");
            h_ = INVALID_HANDLE_VALUE;
        }
    }
private:
    HANDLE h_;
};

class Buffer {
public:
    explicit Buffer(SIZE_T size) : data_(VirtualAlloc(nullptr, size,
                                      MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE)) {
        if (!data_) throw std::runtime_error("VirtualAlloc failed");
    }
    ~Buffer() { VirtualFree(data_, 0, MEM_RELEASE); }
    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;
    void* get() const { return data_; }
private:
    void* data_;
};

[[noreturn]] inline void win_error(const char* operation) {
    throw std::runtime_error(std::string(operation) + " failed, Win32 error " +
                             std::to_string(GetLastError()));
}

inline File open_file(const std::wstring& path, DWORD access, DWORD creation,
               DWORD flags = FILE_ATTRIBUTE_NORMAL) {
    HANDLE h = CreateFileW(path.c_str(), access, 0, nullptr, creation, flags, nullptr);
    if (h == INVALID_HANDLE_VALUE) win_error("CreateFileW");
    return File(h);
}

inline void write_exact(HANDLE h, const void* data, DWORD length) {
    DWORD done = 0;
    if (!WriteFile(h, data, length, &done, nullptr)) win_error("WriteFile");
    if (done != length) throw std::runtime_error("short benchmark write");
}

inline void read_exact(HANDLE h, void* data, DWORD length) {
    DWORD done = 0;
    if (!ReadFile(h, data, length, &done, nullptr)) win_error("ReadFile");
    if (done != length) throw std::runtime_error("short benchmark read");
}

inline void seek(HANDLE h, std::uint64_t offset) {
    LARGE_INTEGER position{};
    position.QuadPart = static_cast<LONGLONG>(offset);
    if (!SetFilePointerEx(h, position, nullptr, FILE_BEGIN)) win_error("SetFilePointerEx");
}

inline void flush(HANDLE h) {
    if (!FlushFileBuffers(h)) win_error("FlushFileBuffers");
}

class Clock {
public:
    Clock() {
        if (!QueryPerformanceFrequency(&frequency_)) win_error("QueryPerformanceFrequency");
    }
    LONGLONG now() const {
        LARGE_INTEGER value{};
        if (!QueryPerformanceCounter(&value)) win_error("QueryPerformanceCounter");
        return value.QuadPart;
    }
    double milliseconds(LONGLONG start) const {
        return 1000.0 * static_cast<double>(now() - start) /
               static_cast<double>(frequency_.QuadPart);
    }
private:
    LARGE_INTEGER frequency_{};
};

} // namespace poolfs::benchmark_io
