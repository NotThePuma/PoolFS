#include "crypto.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace {

void expect(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::string hex(std::span<const std::byte> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string result;
    result.reserve(bytes.size() * 2);
    for (const auto byte : bytes) {
        const auto value = static_cast<unsigned int>(byte);
        result.push_back(digits[value >> 4]);
        result.push_back(digits[value & 15]);
    }
    return result;
}

void test_pbkdf2() {
    poolfs::crypto::Salt salt{};
    for (std::size_t i = 0; i < salt.size(); ++i)
        salt[i] = static_cast<std::byte>("salt"[i % 4]);
    auto keys = poolfs::crypto::derive_keys("password", salt, 2);
    std::array<std::byte, 96> combined{};
    std::copy(keys.xts_key().begin(), keys.xts_key().end(), combined.begin());
    std::copy(keys.mac_key().begin(), keys.mac_key().end(), combined.begin() + 64);
    // Independently generated with .NET's Rfc2898DeriveBytes using SHA-256.
    expect(hex(combined) ==
        "2cf2f46099f4847a19ec26a0d8060ab3ae58d10c670c4f0740e18e13a530376"
        "fb723557f4d10323ee9de72a84b9e6dc2580fd367776206f6b1979d8a88b628"
        "3f9fb06eaeacf3a22486c0ff1a9eeed6e558d236047e6013281eb1d2bf76834c4c",
        "PBKDF2-HMAC-SHA256 vector mismatch");
    auto moved = std::move(keys);
    expect(std::all_of(keys.xts_key().begin(), keys.xts_key().end(),
                       [](std::byte byte) { return byte == std::byte{}; }),
           "moved-from key material was not cleared");
    expect(hex(moved.mac_key()) == hex(std::span<const std::byte>(combined).subspan(64)),
           "moved key material changed");
}

void test_hmac() {
    std::array<std::byte, 20> key{};
    key.fill(std::byte{0x0b});
    constexpr char text[] = "Hi There";
    const auto message = std::span<const std::byte>(
        reinterpret_cast<const std::byte*>(text), sizeof(text) - 1);
    const auto digest = poolfs::crypto::hmac_sha256(key, message);
    // RFC 4231, test case 1.
    expect(hex(digest) ==
           "b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7",
           "HMAC-SHA256 vector mismatch");
    expect(poolfs::crypto::constant_time_equal(digest, digest), "equal MACs differ");
    auto changed = digest;
    changed[0] ^= std::byte{1};
    expect(!poolfs::crypto::constant_time_equal(digest, changed),
           "unequal MACs compare equal");
    expect(!poolfs::crypto::constant_time_equal(
               digest, std::span<const std::byte>(changed).first(31)),
           "different-length MACs compare equal");
}

void test_xts() {
    std::array<std::byte, 64> key{};
    for (std::size_t i = 0; i < key.size(); ++i)
        key[i] = static_cast<std::byte>(i + 1);
    poolfs::crypto::XtsCipher cipher(key);
    std::array<std::byte, poolfs::crypto::kUnitBytes> plain{}, first{}, second{}, restored{};
    for (std::size_t i = 0; i < plain.size(); ++i)
        plain[i] = static_cast<std::byte>((i * 37) & 255);
    cipher.encrypt_unit(17, plain, first);
    cipher.encrypt_unit(18, plain, second);
    expect(first != plain && first != second,
           "XTS encryption did not vary by data unit");
    cipher.decrypt_unit(17, first, restored);
    expect(restored == plain, "XTS round trip mismatch");
    cipher.decrypt_unit(18, first, restored);
    expect(restored != plain, "XTS accepted the wrong data unit sequence");
    bool rejected = false;
    try {
        cipher.encrypt_unit(17, std::span<const std::byte>(plain).first(512), first);
    } catch (const std::invalid_argument&) {
        rejected = true;
    }
    expect(rejected, "XTS accepted a partial data unit");
}

void test_random() {
    poolfs::crypto::Salt first{}, second{};
    poolfs::crypto::random_bytes(first);
    poolfs::crypto::random_bytes(second);
    expect(first != second, "CSPRNG repeated a 32-byte output");
}

} // namespace

int main() {
    try {
        test_random();
        test_pbkdf2();
        test_hmac();
        test_xts();
        std::puts("PoolFS crypto tests passed");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "PoolFS crypto tests failed: %s\n", error.what());
        return 1;
    }
}
