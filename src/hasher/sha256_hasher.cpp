#include "dedupbackup/sha256_hasher.hpp"

#include <openssl/evp.h>

#include <memory>
#include <stdexcept>

namespace dedupbackup {

std::string to_hex(const Digest& d) {
    static const char* kHexDigits = "0123456789abcdef";
    std::string out;
    out.resize(d.size() * 2);
    for (size_t i = 0; i < d.size(); ++i) {
        out[2 * i] = kHexDigits[d[i] >> 4];
        out[2 * i + 1] = kHexDigits[d[i] & 0x0F];
    }
    return out;
}

Digest Sha256Hasher::hash(const uint8_t* data, size_t len) const {
    Digest out{};

    // EVP_MD_CTX must be heap-allocated (its layout is opaque and can
    // change between OpenSSL versions) and explicitly freed — wrap it in
    // unique_ptr with a custom deleter so a thrown exception below can't
    // leak it.
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> ctx(
        EVP_MD_CTX_new(), &EVP_MD_CTX_free);
    if (!ctx) {
        throw std::runtime_error("EVP_MD_CTX_new failed");
    }

    unsigned int out_len = 0;
    const bool ok = EVP_DigestInit_ex(ctx.get(), EVP_sha256(), nullptr) == 1 &&
                     EVP_DigestUpdate(ctx.get(), data, len) == 1 &&
                     EVP_DigestFinal_ex(ctx.get(), out.data(), &out_len) == 1;

    if (!ok || out_len != out.size()) {
        throw std::runtime_error("SHA-256 hashing failed");
    }
    return out;
}

} // namespace dedupbackup
