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

} // namespace poolfs
