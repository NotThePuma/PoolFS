#include "crypto.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>

namespace poolfs::crypto {
namespace {

void check(NTSTATUS status, const char* operation) {
    if (status >= 0) return;
    char message[128];
    std::snprintf(message, sizeof(message), "%s failed (0x%08lx)",
                  operation, static_cast<unsigned long>(status));
    throw std::runtime_error(message);
}

ULONG checked_size(std::size_t size) {
    if (size > std::numeric_limits<ULONG>::max())
        throw std::invalid_argument("crypto input is too large");
    return static_cast<ULONG>(size);
}

void transform(BCRYPT_KEY_HANDLE key, bool encrypt, std::uint64_t unit_number,
               std::span<const std::byte> input, std::span<std::byte> output) {
    if (input.size() != kUnitBytes || output.size() != kUnitBytes)
        throw std::invalid_argument("XTS data unit must be 4096 bytes");
    std::array<UCHAR, 8> sequence{};
    for (std::size_t i = 0; i < sequence.size(); ++i)
        sequence[i] = static_cast<UCHAR>(unit_number >> (8 * i));
    ULONG result = 0;
    auto* source = reinterpret_cast<PUCHAR>(const_cast<std::byte*>(input.data()));
    auto* target = reinterpret_cast<PUCHAR>(output.data());
    const auto status = encrypt ?
        BCryptEncrypt(key, source, kUnitBytes, nullptr, sequence.data(),
                      static_cast<ULONG>(sequence.size()), target, kUnitBytes,
                      &result, 0) :
        BCryptDecrypt(key, source, kUnitBytes, nullptr, sequence.data(),
                      static_cast<ULONG>(sequence.size()), target, kUnitBytes,
                      &result, 0);
    check(status, encrypt ? "BCryptEncrypt(XTS)" : "BCryptDecrypt(XTS)");
    if (result != kUnitBytes)
        throw std::runtime_error("XTS output has unexpected length");
}

} // namespace

void random_bytes(std::span<std::byte> output) {
    auto* position = output.data();
    auto remaining = output.size();
    while (remaining) {
        const auto length = static_cast<ULONG>(
            std::min<std::size_t>(remaining, std::numeric_limits<ULONG>::max()));
        const auto status = BCryptGenRandom(nullptr,
            reinterpret_cast<PUCHAR>(position), length,
            BCRYPT_USE_SYSTEM_PREFERRED_RNG);
        if (status < 0) {
            SecureZeroMemory(output.data(), output.size());
            check(status, "BCryptGenRandom");
        }
        position += length;
        remaining -= length;
    }
}

KeyMaterial::KeyMaterial(KeyMaterial&& other) noexcept : bytes_(other.bytes_) {
    SecureZeroMemory(other.bytes_.data(), other.bytes_.size());
}

KeyMaterial& KeyMaterial::operator=(KeyMaterial&& other) noexcept {
    if (this != &other) {
        SecureZeroMemory(bytes_.data(), bytes_.size());
        bytes_ = other.bytes_;
        SecureZeroMemory(other.bytes_.data(), other.bytes_.size());
    }
    return *this;
}

KeyMaterial::~KeyMaterial() {
    SecureZeroMemory(bytes_.data(), bytes_.size());
}

std::span<const std::byte, 64> KeyMaterial::xts_key() const noexcept {
    return std::span<const std::byte, 64>(bytes_.data(), 64);
}

std::span<const std::byte, 32> KeyMaterial::mac_key() const noexcept {
    return std::span<const std::byte, 32>(bytes_.data() + 64, 32);
}

KeyMaterial derive_keys(std::string_view password_utf8,
                        std::span<const std::byte, 32> salt,
                        std::uint64_t iterations) {
    if (password_utf8.empty() || iterations == 0)
        throw std::invalid_argument("password and iteration count are required");
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                     nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "BCryptOpenAlgorithmProvider(SHA256-HMAC)");
    KeyMaterial derived;
    try {
        check(BCryptDeriveKeyPBKDF2(algorithm,
            reinterpret_cast<PUCHAR>(const_cast<char*>(password_utf8.data())),
            checked_size(password_utf8.size()),
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(salt.data())),
            static_cast<ULONG>(salt.size()), iterations,
            reinterpret_cast<PUCHAR>(derived.bytes_.data()),
            static_cast<ULONG>(derived.bytes_.size()), 0),
            "BCryptDeriveKeyPBKDF2");
    } catch (...) {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return derived;
}

Digest hmac_sha256(std::span<const std::byte> key,
                   std::span<const std::byte> message) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    check(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                     nullptr, BCRYPT_ALG_HANDLE_HMAC_FLAG),
          "BCryptOpenAlgorithmProvider(SHA256-HMAC)");
    Digest digest{};
    try {
        check(BCryptCreateHash(algorithm, &hash, nullptr, 0,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(key.data())),
            checked_size(key.size()), 0), "BCryptCreateHash(HMAC)");
        check(BCryptHashData(hash,
            reinterpret_cast<PUCHAR>(const_cast<std::byte*>(message.data())),
            checked_size(message.size()), 0), "BCryptHashData(HMAC)");
        check(BCryptFinishHash(hash, reinterpret_cast<PUCHAR>(digest.data()),
                               static_cast<ULONG>(digest.size()), 0),
              "BCryptFinishHash(HMAC)");
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return digest;
}

bool constant_time_equal(std::span<const std::byte> left,
                         std::span<const std::byte> right) noexcept {
    if (left.size() != right.size()) return false;
    volatile unsigned int difference = 0;
    for (std::size_t i = 0; i < left.size(); ++i)
        difference = difference | (static_cast<unsigned int>(left[i]) ^
                                   static_cast<unsigned int>(right[i]));
    return difference == 0;
}

XtsCipher::XtsCipher(std::span<const std::byte, 64> key) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_KEY_HANDLE handle = nullptr;
    std::array<UCHAR, 64> secret{};
    std::memcpy(secret.data(), key.data(), secret.size());
    try {
        // Zig's bundled Windows headers omit BCRYPT_XTS_AES_ALGORITHM.
        check(BCryptOpenAlgorithmProvider(&algorithm, L"XTS-AES",
                                         nullptr, 0),
              "BCryptOpenAlgorithmProvider(XTS-AES)");
        check(BCryptGenerateSymmetricKey(algorithm, &handle, nullptr, 0,
                                        secret.data(), static_cast<ULONG>(secret.size()), 0),
              "BCryptGenerateSymmetricKey(XTS)");
        ULONG data_unit_bytes = kUnitBytes;
        check(BCryptSetProperty(handle, BCRYPT_MESSAGE_BLOCK_LENGTH,
                                reinterpret_cast<PUCHAR>(&data_unit_bytes),
                                sizeof(data_unit_bytes), 0),
              "BCryptSetProperty(XTS data unit)");
    } catch (...) {
        SecureZeroMemory(secret.data(), secret.size());
        if (handle) BCryptDestroyKey(handle);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        throw;
    }
    SecureZeroMemory(secret.data(), secret.size());
    algorithm_ = algorithm;
    key_ = handle;
}

XtsCipher::~XtsCipher() {
    if (key_) BCryptDestroyKey(static_cast<BCRYPT_KEY_HANDLE>(key_));
    if (algorithm_) BCryptCloseAlgorithmProvider(static_cast<BCRYPT_ALG_HANDLE>(algorithm_), 0);
}

void XtsCipher::encrypt_unit(std::uint64_t unit_number,
                             std::span<const std::byte> plaintext,
                             std::span<std::byte> ciphertext) {
    transform(static_cast<BCRYPT_KEY_HANDLE>(key_), true,
              unit_number, plaintext, ciphertext);
}

void XtsCipher::decrypt_unit(std::uint64_t unit_number,
                             std::span<const std::byte> ciphertext,
                             std::span<std::byte> plaintext) {
    transform(static_cast<BCRYPT_KEY_HANDLE>(key_), false,
              unit_number, ciphertext, plaintext);
}

} // namespace poolfs::crypto
