#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <string>
#include <string_view>
#include <utility>

namespace poolfs::password {

class Secret {
public:
    explicit Secret(std::string&& value);
    ~Secret();
    Secret(const Secret&) = delete;
    Secret& operator=(const Secret&) = delete;
    Secret(Secret&& other) noexcept;
    Secret& operator=(Secret&& other) noexcept;

    std::string_view view() const noexcept { return value_; }

private:
    std::string value_;
};

// Requires an interactive Windows console. Neither passwords nor derived keys
// belong on a command line, in an environment variable, or in a log.
Secret prompt(const wchar_t* label, bool confirm = false);

// The controller uses one inherited, allowlisted anonymous pipe handle to
// deliver a password to its hidden mount process. The handle is not a secret.
Secret read_pipe(HANDLE pipe);
void write_pipe(HANDLE pipe, std::string_view secret);

} // namespace poolfs::password
