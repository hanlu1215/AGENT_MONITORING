#include "util/crypto.h"

#include <windows.h>
#include <bcrypt.h>

#include <mutex>
#include <random>

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif

namespace cn {
namespace {

// BCrypt 的一次性哈希（SHA-1 / SHA-256；HMAC 用 hmac=true）
std::vector<unsigned char> HashOnce(LPCWSTR algorithm, const std::string& data, const char* key,
                                    size_t key_len, bool hmac, std::vector<unsigned char>* out_ok) {
    (void)out_ok;
    BCRYPT_ALG_HANDLE alg = nullptr;
    ULONG flags = hmac ? BCRYPT_ALG_HANDLE_HMAC_FLAG : 0;
    if (!NT_SUCCESS(BCryptOpenAlgorithmProvider(&alg, algorithm, nullptr, flags))) {
        return {};
    }
    std::vector<unsigned char> hash;
    BCRYPT_HASH_HANDLE ctx = nullptr;
    DWORD object_size = 0;
    DWORD hash_size = 0;
    DWORD produced = 0;
    bool ok = NT_SUCCESS(BCryptGetProperty(alg, BCRYPT_OBJECT_LENGTH, (PUCHAR)&object_size,
                                           sizeof(object_size), &produced, 0)) &&
              NT_SUCCESS(BCryptGetProperty(alg, BCRYPT_HASH_LENGTH, (PUCHAR)&hash_size,
                                           sizeof(hash_size), &produced, 0));
    if (ok) {
        std::vector<unsigned char> object(object_size);
        NTSTATUS status = BCryptCreateHash(alg, &ctx, object.data(), object_size,
                                           hmac ? (PUCHAR)key : nullptr,
                                           hmac ? ULONG(key_len) : 0, 0);
        if (NT_SUCCESS(status)) {
            status = BCryptHashData(ctx, (PUCHAR)data.data(), ULONG(data.size()), 0);
            if (NT_SUCCESS(status)) {
                hash.resize(hash_size);
                status = BCryptFinishHash(ctx, hash.data(), hash_size, 0);
                if (!NT_SUCCESS(status)) hash.clear();
            }
            BCryptDestroyHash(ctx);
        }
    }
    BCryptCloseAlgorithmProvider(alg, 0);
    return hash;
}

}  // namespace

std::vector<unsigned char> Sha1(const std::string& data) {
    return HashOnce(BCRYPT_SHA1_ALGORITHM, data, nullptr, 0, false, nullptr);
}

std::vector<unsigned char> Sha256Bytes(const std::string& data) {
    return HashOnce(BCRYPT_SHA256_ALGORITHM, data, nullptr, 0, false, nullptr);
}

std::vector<unsigned char> HmacSha256(const std::string& key, const std::string& message) {
    return HashOnce(BCRYPT_SHA256_ALGORITHM, message, key.data(), key.size(), true, nullptr);
}

static std::string ToHex(const std::vector<unsigned char>& data) {
    static const char* kDigits = "0123456789abcdef";
    std::string out;
    out.reserve(data.size() * 2);
    for (unsigned char byte : data) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::string Sha1Hex(const std::string& data) { return ToHex(Sha1(data)); }
std::string Sha256Hex(const std::string& data) { return ToHex(Sha256Bytes(data)); }

std::string Base64Encode(const std::vector<unsigned char>& data) {
    static const char* kAlphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((data.size() + 2) / 3 * 4);
    size_t index = 0;
    while (index + 2 < data.size()) {
        unsigned value = (unsigned(data[index]) << 16) | (unsigned(data[index + 1]) << 8) |
                         unsigned(data[index + 2]);
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(kAlphabet[(value >> 6) & 0x3F]);
        out.push_back(kAlphabet[value & 0x3F]);
        index += 3;
    }
    if (index + 1 == data.size()) {
        unsigned value = unsigned(data[index]) << 16;
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (index + 2 == data.size()) {
        unsigned value = (unsigned(data[index]) << 16) | (unsigned(data[index + 1]) << 8);
        out.push_back(kAlphabet[(value >> 18) & 0x3F]);
        out.push_back(kAlphabet[(value >> 12) & 0x3F]);
        out.push_back(kAlphabet[(value >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

int RandomInt(int low, int high) {
    if (high <= low) return low;
    static std::mt19937 generator{std::random_device{}()};
    static std::mutex lock;
    std::lock_guard<std::mutex> guard(lock);
    std::uniform_int_distribution<int> dist(low, high);
    return dist(generator);
}

}  // namespace cn
