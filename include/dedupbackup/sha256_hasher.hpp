#pragma once

#include "dedupbackup/hasher.hpp"

namespace dedupbackup {

// SHA-256 via OpenSSL's EVP API (not the legacy SHA256_Init/Update/Final
// functions, which OpenSSL 3.x deprecates in favor of EVP_Digest*).
class Sha256Hasher final : public IHasher {
public:
    Digest hash(const uint8_t* data, size_t len) const override;
};

} // namespace dedupbackup
