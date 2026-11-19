#include "mount_config.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

namespace poolfs {
namespace {

constexpr std::size_t kMaximumConfigBytes = 1024 * 1024;
constexpr std::size_t kMaximumMembers = 16;

[[noreturn]] void invalid(std::size_t offset, std::string_view reason) {
    throw std::runtime_error("invalid PoolFS mount config at byte " +
        std::to_string(offset) + ": " + std::string(reason));
}

std::string read_config(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("cannot open PoolFS mount config");
    const auto end = input.tellg();
    if (end < 0 || end > static_cast<std::streamoff>(kMaximumConfigBytes))
        throw std::runtime_error("PoolFS mount config exceeds 1 MiB limit");
    const auto length = static_cast<std::size_t>(end);
    std::string bytes(length, '\0');
    input.seekg(0);
    if (!input.read(bytes.data(), static_cast<std::streamsize>(length)))
        throw std::runtime_error("cannot read PoolFS mount config");
    char extra = 0;
    if (input.get(extra))
        throw std::runtime_error("PoolFS mount config changed while reading");
    if (!input.eof())
        throw std::runtime_error("cannot finish reading PoolFS mount config");
    return bytes;
}

namespace fs = std::filesystem;

struct UniqueHandle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~UniqueHandle() { reset(); }
    UniqueHandle() = default;
    UniqueHandle(const UniqueHandle&) = delete;
    UniqueHandle& operator=(const UniqueHandle&) = delete;
    void reset(HANDLE next = INVALID_HANDLE_VALUE) {
        if (value != INVALID_HANDLE_VALUE) CloseHandle(value);
        value = next;
    }
};

[[noreturn]] void windows_error(const char* action) {
    throw std::system_error(static_cast<int>(GetLastError()),
                            std::system_category(), action);
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
    wchar_t drive = config.drive;
    if (drive >= L'a' && drive <= L'z') drive -= L'a' - L'A';
    if (drive < L'A' || drive > L'Z')
        throw std::invalid_argument("drive must be an A-Z letter");
    validate_pool_name(config.name);
    if (config.members.empty() || config.members.size() > kMaximumMembers)
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
    if (output.size() > kMaximumConfigBytes)
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

class JsonParser {
public:
    explicit JsonParser(std::string_view source) : source_(source) {
        if (source_.starts_with("\xEF\xBB\xBF")) position_ = 3;
    }

    MountConfig parse(const std::filesystem::path& config_directory) {
        skip_space();
        expect('{');
        bool schema_seen = false;
        bool drive_seen = false;
        bool members_seen = false;
        bool encryption_seen = false;
        bool name_seen = false;
        MountConfig config;
        skip_space();
        if (!consume('}')) {
            for (;;) {
                const auto key = parse_string();
                skip_space();
                expect(':');
                skip_space();
                if (key == L"schema_version") {
                    if (schema_seen) fail("duplicate schema_version field");
                    schema_seen = true;
                    expect('1');
                } else if (key == L"name") {
                    if (name_seen) fail("duplicate name field");
                    name_seen = true;
                    config.name = parse_string();
                    validate_pool_name(config.name);
                } else if (key == L"drive") {
                    if (drive_seen) fail("duplicate drive field");
                    drive_seen = true;
                    const auto drive = parse_string();
                    if (drive.size() != 2 || drive[1] != L':' ||
                        !((drive[0] >= L'A' && drive[0] <= L'Z') ||
                          (drive[0] >= L'a' && drive[0] <= L'z')))
                        fail("drive must be a letter such as P:");
                    config.drive = drive[0] >= L'a' ?
                        wchar_t(drive[0] - (L'a' - L'A')) : drive[0];
                } else if (key == L"members") {
                    if (members_seen) fail("duplicate members field");
                    members_seen = true;
                    config.members = parse_members(config_directory);
                } else if (key == L"require_encrypted") {
                    if (encryption_seen) fail("duplicate require_encrypted field");
                    encryption_seen = true;
                    config.require_encrypted = parse_boolean();
                } else {
                    fail("unknown field (passwords and keys are not allowed)");
                }
                skip_space();
                if (consume('}')) break;
                expect(',');
                skip_space();
            }
        }
        skip_space();
        if (position_ != source_.size()) fail("trailing content");
        if (!schema_seen || !drive_seen || !members_seen)
            fail("schema_version, drive, and members are required");
        return config;
    }

private:
    std::string_view source_;
    std::size_t position_ = 0;

    [[noreturn]] void fail(std::string_view reason) const {
        invalid(position_, reason);
    }

    void skip_space() {
        while (position_ < source_.size()) {
            const auto c = source_[position_];
            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') break;
            ++position_;
        }
    }

    bool consume(char expected) {
        if (position_ < source_.size() && source_[position_] == expected) {
            ++position_;
            return true;
        }
        return false;
    }

    void expect(char expected) {
        if (!consume(expected)) fail("unexpected JSON token");
    }

    void literal(std::string_view expected) {
        if (source_.substr(position_, expected.size()) != expected)
            fail("unexpected JSON value");
        position_ += expected.size();
    }

    bool parse_boolean() {
        if (source_.substr(position_).starts_with("true")) {
            literal("true");
            return true;
        }
        literal("false");
        return false;
    }

    unsigned hex4() {
        if (source_.size() - position_ < 4) fail("incomplete Unicode escape");
        unsigned value = 0;
        for (int i = 0; i < 4; ++i) {
            const auto c = source_[position_++];
            value <<= 4;
            if (c >= '0' && c <= '9') value |= unsigned(c - '0');
            else if (c >= 'a' && c <= 'f') value |= unsigned(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') value |= unsigned(c - 'A' + 10);
            else fail("invalid Unicode escape");
        }
        return value;
    }

    void append_scalar(std::wstring& output, std::uint32_t scalar) {
        if (scalar > 0x10ffff || (scalar >= 0xd800 && scalar <= 0xdfff))
            fail("invalid Unicode scalar");
        if (scalar <= 0xffff) {
            output.push_back(static_cast<wchar_t>(scalar));
        } else {
            scalar -= 0x10000;
            output.push_back(static_cast<wchar_t>(0xd800 + (scalar >> 10)));
            output.push_back(static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff)));
        }
    }

    void append_utf8(std::wstring& output) {
        const auto first = static_cast<unsigned char>(source_[position_++]);
        std::uint32_t scalar = 0;
        int remaining = 0;
        std::uint32_t minimum = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            scalar = first & 0x1f;
            remaining = 1;
            minimum = 0x80;
        } else if (first >= 0xe0 && first <= 0xef) {
            scalar = first & 0x0f;
            remaining = 2;
            minimum = 0x800;
        } else if (first >= 0xf0 && first <= 0xf4) {
            scalar = first & 0x07;
            remaining = 3;
            minimum = 0x10000;
        } else {
            fail("invalid UTF-8 sequence");
        }
        while (remaining--) {
            if (position_ == source_.size()) fail("incomplete UTF-8 sequence");
            const auto next = static_cast<unsigned char>(source_[position_++]);
            if ((next & 0xc0) != 0x80) fail("invalid UTF-8 continuation");
            scalar = (scalar << 6) | (next & 0x3f);
        }
        if (scalar < minimum) fail("overlong UTF-8 sequence");
        append_scalar(output, scalar);
    }

    std::wstring parse_string() {
        expect('"');
        std::wstring result;
        while (position_ < source_.size()) {
            const auto c = static_cast<unsigned char>(source_[position_++]);
            if (c == '"') return result;
            if (c < 0x20) fail("unescaped control character");
            if (c == '\\') {
                if (position_ == source_.size()) fail("incomplete string escape");
                const auto escaped = source_[position_++];
                switch (escaped) {
                case '"': result.push_back(L'"'); break;
                case '\\': result.push_back(L'\\'); break;
                case '/': result.push_back(L'/'); break;
                case 'b': result.push_back(L'\b'); break;
                case 'f': result.push_back(L'\f'); break;
                case 'n': result.push_back(L'\n'); break;
                case 'r': result.push_back(L'\r'); break;
                case 't': result.push_back(L'\t'); break;
                case 'u': {
                    const auto upper = hex4();
                    if (upper >= 0xd800 && upper <= 0xdbff) {
                        if (!consume('\\') || !consume('u'))
                            fail("high surrogate requires low surrogate");
                        const auto lower = hex4();
                        if (lower < 0xdc00 || lower > 0xdfff)
                            fail("invalid low surrogate");
                        result.push_back(static_cast<wchar_t>(upper));
                        result.push_back(static_cast<wchar_t>(lower));
                    } else if (upper >= 0xdc00 && upper <= 0xdfff) {
                        fail("unexpected low surrogate");
                    } else {
                        result.push_back(static_cast<wchar_t>(upper));
                    }
                    break;
                }
                default: fail("invalid string escape");
                }
            } else if (c < 0x80) {
                result.push_back(static_cast<wchar_t>(c));
            } else {
                --position_;
                append_utf8(result);
            }
        }
        fail("unterminated string");
    }

    std::filesystem::path member_path(const std::filesystem::path& directory,
                                      const std::wstring& text) {
        if (text.empty()) fail("member path is empty");
        for (const auto c : text)
            if (c < 0x20) fail("member path contains a control character");
        std::wstring windows_path(text);
        std::replace(windows_path.begin(), windows_path.end(), L'/', L'\\');
        if (windows_path.starts_with(L"\\\\?\\") ||
            windows_path.starts_with(L"\\\\.\\"))
            fail("device namespace paths are not member image paths");
        if (windows_path.starts_with(L"\\\\")) {
            const auto server_end = windows_path.find(L'\\', 2);
            const auto share_end = server_end == std::wstring::npos ?
                std::wstring::npos : windows_path.find(L'\\', server_end + 1);
            if (server_end == 2 || server_end == std::wstring::npos ||
                share_end == server_end + 1 || share_end == std::wstring::npos ||
                share_end + 1 == windows_path.size())
                fail("UNC member path must include server, share, and image file");
        }
        const std::filesystem::path input(text);
        if (input.has_root_name() != input.has_root_directory() ||
            (input.has_root_directory() && !input.has_root_name()))
            fail("member path has an incomplete Windows root");
        auto result = input.is_absolute() ? input : directory / input;
        result = result.lexically_normal();
        result.make_preferred();
        if (result.empty() || result.filename().empty())
            fail("member path must name an image file");
        return result;
    }

    std::vector<std::filesystem::path> parse_members(
            const std::filesystem::path& directory) {
        expect('[');
        std::vector<std::filesystem::path> members;
        skip_space();
        if (!consume(']')) {
            for (;;) {
                auto path = member_path(directory, parse_string());
                const auto native = path.native();
                for (const auto& previous : members) {
                    const auto prior = previous.native();
                    if (CompareStringOrdinal(native.c_str(),
                            static_cast<int>(native.size()), prior.c_str(),
                            static_cast<int>(prior.size()), TRUE) == CSTR_EQUAL)
                        fail("duplicate member path");
                }
                members.push_back(std::move(path));
                if (members.size() > kMaximumMembers) fail("too many members");
                skip_space();
                if (consume(']')) break;
                expect(',');
                skip_space();
            }
        }
        if (members.empty()) fail("members must contain 1 to 16 paths");
        return members;
    }
};

} // namespace

void validate_pool_name(std::wstring_view name) {
    if (name.empty() || name.size() > 32)
        throw std::invalid_argument("pool name must contain 1 to 32 UTF-16 code units");
    if (name.front() == L' ' || name.back() == L' ' || name.back() == L'.')
        throw std::invalid_argument("pool name cannot start or end with a space or end with a period");
    constexpr std::wstring_view invalid_characters = L"<>:/\\|?*\"";
    for (std::size_t i = 0; i < name.size(); ++i) {
        const auto character = name[i];
        if (character < 0x20 || character == 0x7f ||
            invalid_characters.find(character) != std::wstring_view::npos)
            throw std::invalid_argument("pool name contains a character not allowed in a Windows volume label");
        if (character >= 0xd800 && character <= 0xdbff) {
            if (++i >= name.size() || name[i] < 0xdc00 || name[i] > 0xdfff)
                throw std::invalid_argument("pool name contains an invalid UTF-16 surrogate pair");
        } else if (character >= 0xdc00 && character <= 0xdfff) {
            throw std::invalid_argument("pool name contains an invalid UTF-16 surrogate pair");
        }
    }
}

MountConfig load_mount_config(const std::filesystem::path& config_path) {
    if (config_path.empty())
        throw std::invalid_argument("PoolFS mount config path is empty");
    const auto absolute = std::filesystem::absolute(config_path).lexically_normal();
    const auto bytes = read_config(absolute);
    return JsonParser(bytes).parse(absolute.parent_path());
}

bool mount_paths_alias(const fs::path& target, const fs::path& member) {
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
        if (mount_paths_alias(target, absolute_member))
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

} // namespace poolfs
