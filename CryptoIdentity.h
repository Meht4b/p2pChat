#pragma once

#include <asio/ssl.hpp>

#include <memory>
#include <string>

#include <openssl/evp.h>
#include <openssl/x509.h>

#include "PeerId.h"

class StorageManager;

// Long-lived signing identity used by TLS. The peer id is the SHA-256 digest of
// the DER SubjectPublicKeyInfo, so a handshake cannot claim another identity.
class CryptoIdentity
{
public:
    CryptoIdentity() = default;

    static CryptoIdentity generate();
    static CryptoIdentity loadOrCreate(StorageManager& storage);

    const PeerId& peerId() const noexcept { return peer_id_; }
    std::shared_ptr<asio::ssl::context> makeTlsContext() const;
    static PeerId peerIdFromTlsPeer(SSL* ssl);

private:
    using PrivateKey = std::shared_ptr<EVP_PKEY>;
    using Certificate = std::shared_ptr<X509>;

    CryptoIdentity(PrivateKey key, Certificate certificate);
    static PeerId peerIdFromKey(EVP_PKEY* key);

    PrivateKey key_;
    Certificate certificate_;
    PeerId peer_id_;
};
