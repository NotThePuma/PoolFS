#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace poolfs::crypto {

constexpr std::size_t kUnitBytes = 4096;
using Salt = std::array<std::byte, 32>;
using Digest = std::array<std::byte, 32>;

void random_bytes(std::span<std::byte> output);

class KeyMaterial {
public:
    KeyMaterial(const KeyMaterial&) = delete;
    KeyMaterial& operator=(const KeyMaterial&) = delete;
    KeyMaterial(KeyMaterial&& other) noexcept;
    KeyMaterial& operator=(KeyMaterial&& other) noexcept;
    ~KeyMaterial();

    std::span<const std::byte, 64> xts_key() const noexcept;
    std::span<const std::byte, 32> mac_key() const noexcept;

private:
    KeyMaterial() = default;
    friend KeyMaterial derive_keys(std::string_view password_utf8,
                                   std::span<const std::byte, 32> salt,
                                   std::uint64_t iterations);
    std::array<std::byte, 96> bytes_{};
};

KeyMaterial derive_keys(std::string_view password_utf8,
                        std::span<const std::byte, 32> salt,
                        std::uint64_t iterations);

Digest hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> message);

bool constant_time_equal(std::span<const std::byte> left,
                         std::span<const std::byte> right) noexcept;

class XtsCipher {
public:
    explicit XtsCipher(std::span<const std::byte, 64> key);
    XtsCipher(const XtsCipher&) = delete;
    XtsCipher& operator=(const XtsCipher&) = delete;
    XtsCipher(XtsCipher&&) = delete;
    XtsCipher& operator=(XtsCipher&&) = delete;
    ~XtsCipher();

    void encrypt_unit(std::uint64_t unit_number,
                      std::span<const std::byte> plaintext,
                      std::span<std::byte> ciphertext);
    void decrypt_unit(std::uint64_t unit_number,
                      std::span<const std::byte> ciphertext,
                      std::span<std::byte> plaintext);

private:
    void* algorithm_ = nullptr;
    void* key_ = nullptr;
};

} // namespace poolfs::crypto
