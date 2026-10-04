// Copyright (c) 2026 Zhiyi IME Contributors. GPL-3.0-only.
//
// SHA-256 and the ECDSA P-256 manifest signature, with Windows CNG (bcrypt).

#include <cxxime/update.h>

#include <bcrypt.h>
#include <wincrypt.h>

#include <vector>

#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")

namespace cxxime {
namespace update {
namespace {

constexpr char kBuiltinPublicKey[] =
#include "update_public_key.inc"
    ;

bool decode_base64(const std::string& text, std::vector<unsigned char>* bytes) {
    DWORD length = 0;
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              nullptr, &length, nullptr, nullptr)) {
        return false;
    }
    bytes->resize(length);
    if (!CryptStringToBinaryA(text.c_str(), static_cast<DWORD>(text.size()), CRYPT_STRING_BASE64,
                              bytes->data(), &length, nullptr, nullptr)) {
        return false;
    }
    bytes->resize(length);
    return true;
}

class Sha256 {
public:
    Sha256() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0 ||
            BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            hash_ = nullptr;
        }
    }
    ~Sha256() {
        if (hash_) BCryptDestroyHash(hash_);
        if (algorithm_) BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    Sha256(const Sha256&) = delete;
    Sha256& operator=(const Sha256&) = delete;

    bool add(const void* data, size_t size) {
        return hash_ && BCryptHashData(hash_, static_cast<PUCHAR>(const_cast<void*>(data)),
                                       static_cast<ULONG>(size), 0) >= 0;
    }
    bool finish(unsigned char (&digest)[32]) {
        return hash_ && BCryptFinishHash(hash_, digest, sizeof(digest), 0) >= 0;
    }

private:
    BCRYPT_ALG_HANDLE algorithm_ = nullptr;
    BCRYPT_HASH_HANDLE hash_ = nullptr;
};

std::string to_hex(const unsigned char* bytes, size_t size) {
    static const char kDigits[] = "0123456789abcdef";
    std::string hex;
    hex.reserve(size * 2);
    for (size_t i = 0; i < size; ++i) {
        hex.push_back(kDigits[bytes[i] >> 4]);
        hex.push_back(kDigits[bytes[i] & 15]);
    }
    return hex;
}

}  // namespace

const std::string& builtin_public_key() {
    static const std::string key = kBuiltinPublicKey;
    return key;
}

bool verify_signature(const std::string& data, const std::string& signature_base64,
                      const std::string& public_key_base64) {
    std::vector<unsigned char> signature;
    std::vector<unsigned char> key;
    if (!decode_base64(signature_base64, &signature) || signature.size() != 64 ||
        !decode_base64(public_key_base64, &key) || key.size() != 64) {
        return false;
    }
    unsigned char digest[32];
    Sha256 sha;
    if (!sha.add(data.data(), data.size()) || !sha.finish(digest)) return false;

    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_ECDSA_P256_ALGORITHM, nullptr, 0) < 0) {
        return false;
    }
    std::vector<unsigned char> blob(sizeof(BCRYPT_ECCKEY_BLOB) + key.size());
    auto* header = reinterpret_cast<BCRYPT_ECCKEY_BLOB*>(blob.data());
    header->dwMagic = BCRYPT_ECDSA_PUBLIC_P256_MAGIC;
    header->cbKey = 32;
    std::copy(key.begin(), key.end(), blob.begin() + sizeof(BCRYPT_ECCKEY_BLOB));
    BCRYPT_KEY_HANDLE handle = nullptr;
    bool valid = false;
    if (BCryptImportKeyPair(algorithm, nullptr, BCRYPT_ECCPUBLIC_BLOB, &handle, blob.data(),
                            static_cast<ULONG>(blob.size()), 0) >= 0) {
        valid = BCryptVerifySignature(handle, nullptr, digest, sizeof(digest), signature.data(),
                                      static_cast<ULONG>(signature.size()), 0) >= 0;
        BCryptDestroyKey(handle);
    }
    BCryptCloseAlgorithmProvider(algorithm, 0);
    return valid;
}

bool sha256_file(HANDLE file, std::string* hex) {
    LARGE_INTEGER start = {};
    if (file == INVALID_HANDLE_VALUE || !SetFilePointerEx(file, start, nullptr, FILE_BEGIN)) {
        return false;
    }
    Sha256 sha;
    std::vector<unsigned char> buffer(1 << 20);
    for (;;) {
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            return false;
        }
        if (read == 0) break;
        if (!sha.add(buffer.data(), read)) return false;
    }
    unsigned char digest[32];
    if (!sha.finish(digest)) return false;
    *hex = to_hex(digest, sizeof(digest));
    return true;
}

bool sha256_file(const std::wstring& path, std::string* hex) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    const bool hashed = sha256_file(file, hex);
    CloseHandle(file);
    return hashed;
}

}  // namespace update
}  // namespace cxxime
