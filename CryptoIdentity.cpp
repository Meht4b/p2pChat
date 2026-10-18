#include "CryptoIdentity.h"

#include "StorageManager.h"

#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/sha.h>
#include <openssl/x509v3.h>

#include <array>
#include <climits>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <dpapi.h>
#include <wincrypt.h>
#pragma comment(lib, "Crypt32.lib")
#endif

namespace {

[[noreturn]] void cryptoError(const char* operation)
{
    const unsigned long code = ERR_get_error();
    std::array<char, 256> detail{};
    if (code) ERR_error_string_n(code, detail.data(), detail.size());
    throw std::runtime_error(std::string(operation) + (code ? ": " + std::string(detail.data()) : " failed"));
}

using BioPtr = std::unique_ptr<BIO, decltype(&BIO_free)>;

std::string base64Encode(const unsigned char* data, std::size_t size)
{
    if (size > static_cast<std::size_t>(INT_MAX)) throw std::length_error("Identity data too large");
    std::string result(4 * ((size + 2) / 3), '\0');
    const int written = EVP_EncodeBlock(reinterpret_cast<unsigned char*>(result.data()), data,
                                        static_cast<int>(size));
    if (written < 0) cryptoError("Encoding identity");
    result.resize(static_cast<std::size_t>(written));
    return result;
}

std::vector<unsigned char> base64Decode(const std::string& text)
{
    if (text.empty() || text.size() % 4 != 0 || text.size() > static_cast<std::size_t>(INT_MAX))
        throw std::runtime_error("Stored cryptographic identity is malformed");
    std::vector<unsigned char> result(3 * (text.size() / 4));
    const int written = EVP_DecodeBlock(result.data(),
        reinterpret_cast<const unsigned char*>(text.data()), static_cast<int>(text.size()));
    if (written < 0) throw std::runtime_error("Stored cryptographic identity is malformed");
    std::size_t actual = static_cast<std::size_t>(written);
    if (!text.empty() && text.back() == '=') --actual;
    if (text.size() > 1 && text[text.size() - 2] == '=') --actual;
    result.resize(actual);
    return result;
}

std::vector<unsigned char> protectPrivateKey(const std::vector<unsigned char>& plain)
{
#ifdef _WIN32
    DATA_BLOB input{ static_cast<DWORD>(plain.size()), const_cast<BYTE*>(plain.data()) };
    static constexpr char entropyText[] = "p2pChat identity v1";
    DATA_BLOB entropy{ static_cast<DWORD>(sizeof(entropyText) - 1),
                       reinterpret_cast<BYTE*>(const_cast<char*>(entropyText)) };
    DATA_BLOB output{};
    if (!CryptProtectData(&input, L"p2pChat identity", &entropy, nullptr, nullptr,
                          CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Windows could not protect the local identity key");
    std::vector<unsigned char> protectedBytes(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return protectedBytes;
#else
    (void)plain;
    throw std::runtime_error("Persistent cryptographic identities currently require Windows DPAPI");
#endif
}

std::vector<unsigned char> unprotectPrivateKey(const std::vector<unsigned char>& protectedBytes)
{
#ifdef _WIN32
    DATA_BLOB input{ static_cast<DWORD>(protectedBytes.size()),
                     const_cast<BYTE*>(protectedBytes.data()) };
    static constexpr char entropyText[] = "p2pChat identity v1";
    DATA_BLOB entropy{ static_cast<DWORD>(sizeof(entropyText) - 1),
                       reinterpret_cast<BYTE*>(const_cast<char*>(entropyText)) };
    DATA_BLOB output{};
    if (!CryptUnprotectData(&input, nullptr, &entropy, nullptr, nullptr,
                            CRYPTPROTECT_UI_FORBIDDEN, &output))
        throw std::runtime_error("Windows could not unlock the local identity key");
    std::vector<unsigned char> plain(output.pbData, output.pbData + output.cbData);
    SecureZeroMemory(output.pbData, output.cbData);
    LocalFree(output.pbData);
    return plain;
#else
    (void)protectedBytes;
    throw std::runtime_error("Persistent cryptographic identities currently require Windows DPAPI");
#endif
}

std::string certificateToPem(X509* certificate)
{
    BioPtr bio(BIO_new(BIO_s_mem()), BIO_free);
    if (!bio || PEM_write_bio_X509(bio.get(), certificate) != 1) cryptoError("Serializing identity certificate");
    char* data = nullptr;
    const long size = BIO_get_mem_data(bio.get(), &data);
    if (size <= 0 || !data) cryptoError("Reading identity certificate");
    return std::string(data, static_cast<std::size_t>(size));
}

} // namespace

CryptoIdentity::CryptoIdentity(PrivateKey key, Certificate certificate)
    : key_(std::move(key)), certificate_(std::move(certificate)),
      peer_id_(peerIdFromKey(key_.get()))
{
    if (!key_ || !certificate_ || X509_check_private_key(certificate_.get(), key_.get()) != 1)
        throw std::runtime_error("Cryptographic identity certificate and key do not match");
    if (EVP_PKEY_base_id(key_.get()) != EVP_PKEY_ED25519)
        throw std::runtime_error("Identity key must use Ed25519");
}

CryptoIdentity CryptoIdentity::generate()
{
    EVP_PKEY_CTX* rawContext = EVP_PKEY_CTX_new_id(EVP_PKEY_ED25519, nullptr);
    if (!rawContext) cryptoError("Creating identity key generator");
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(rawContext, EVP_PKEY_CTX_free);
    EVP_PKEY* rawKey = nullptr;
    if (EVP_PKEY_keygen_init(context.get()) <= 0 || EVP_PKEY_keygen(context.get(), &rawKey) <= 0)
        cryptoError("Generating identity key");
    PrivateKey key(rawKey, EVP_PKEY_free);

    X509* rawCertificate = X509_new();
    if (!rawCertificate) cryptoError("Creating identity certificate");
    Certificate certificate(rawCertificate, X509_free);
    if (X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -300) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 1576800000L) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1)
        cryptoError("Building identity certificate");

    X509_NAME* subject = X509_get_subject_name(certificate.get());
    if (!subject || X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("p2pChat peer"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_sign(certificate.get(), key.get(), nullptr) <= 0)
        cryptoError("Signing identity certificate");

    return CryptoIdentity(std::move(key), std::move(certificate));
}

CryptoIdentity CryptoIdentity::loadOrCreate(StorageManager& storage)
{
    constexpr const char* identitySetting = "crypto.identity.v1";
    if (const auto record = storage.getSetting(identitySetting)) {
        const std::size_t separator = record->find('\n');
        if (separator == std::string::npos || separator == 0 || separator + 1 == record->size())
            throw std::runtime_error("The saved cryptographic identity is malformed");
        auto protectedBytes = base64Decode(record->substr(0, separator));
        auto der = unprotectPrivateKey(protectedBytes);
        const unsigned char* cursor = der.data();
        EVP_PKEY* rawKey = d2i_AutoPrivateKey(nullptr, &cursor, static_cast<long>(der.size()));
        if (!rawKey || cursor != der.data() + der.size()) cryptoError("Loading identity key");
        PrivateKey key(rawKey, EVP_PKEY_free);

        const std::string certificatePem = record->substr(separator + 1);
        BioPtr bio(BIO_new_mem_buf(certificatePem.data(), static_cast<int>(certificatePem.size())), BIO_free);
        X509* rawCertificate = bio ? PEM_read_bio_X509(bio.get(), nullptr, nullptr, nullptr) : nullptr;
        if (!rawCertificate) cryptoError("Loading identity certificate");
        OPENSSL_cleanse(der.data(), der.size());
        return CryptoIdentity(std::move(key), Certificate(rawCertificate, X509_free));
    }

    CryptoIdentity identity = generate();
    int derSize = i2d_PrivateKey(identity.key_.get(), nullptr);
    if (derSize <= 0) cryptoError("Serializing identity key");
    std::vector<unsigned char> der(static_cast<std::size_t>(derSize));
    unsigned char* cursor = der.data();
    if (i2d_PrivateKey(identity.key_.get(), &cursor) != derSize) cryptoError("Serializing identity key");
    const auto protectedBytes = protectPrivateKey(der);
    OPENSSL_cleanse(der.data(), der.size());
    const std::string record = base64Encode(protectedBytes.data(), protectedBytes.size()) +
        "\n" + certificateToPem(identity.certificate_.get());
    storage.setSetting(identitySetting, record);
    return identity;
}

std::shared_ptr<asio::ssl::context> CryptoIdentity::makeTlsContext() const
{
    if (!key_ || !certificate_) throw std::logic_error("Cryptographic identity is not initialized");
    auto context = std::make_shared<asio::ssl::context>(asio::ssl::context::tlsv13);
    if (SSL_CTX_set_min_proto_version(context->native_handle(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_set_max_proto_version(context->native_handle(), TLS1_3_VERSION) != 1 ||
        SSL_CTX_use_certificate(context->native_handle(), certificate_.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context->native_handle(), key_.get()) != 1 ||
        SSL_CTX_check_private_key(context->native_handle()) != 1)
        cryptoError("Configuring TLS 1.3 identity");

    context->set_verify_mode(asio::ssl::verify_peer | asio::ssl::verify_fail_if_no_peer_cert);
    // Trust is self-certifying: after TLS proves possession of this key, Session
    // requires the application peer id to equal SHA-256(SubjectPublicKeyInfo).
    context->set_verify_callback([](bool, asio::ssl::verify_context&) { return true; });
    return context;
}

PeerId CryptoIdentity::peerIdFromKey(EVP_PKEY* key)
{
    const int size = key ? i2d_PUBKEY(key, nullptr) : 0;
    if (size <= 0) cryptoError("Encoding identity public key");
    std::vector<unsigned char> der(static_cast<std::size_t>(size));
    unsigned char* cursor = der.data();
    if (i2d_PUBKEY(key, &cursor) != size) cryptoError("Encoding identity public key");
    PeerId::Bytes id{};
    SHA256(der.data(), der.size(), id.data());
    return PeerId(id);
}

PeerId CryptoIdentity::peerIdFromTlsPeer(SSL* ssl)
{
    if (!ssl) throw std::runtime_error("TLS session is missing");
#if OPENSSL_VERSION_NUMBER >= 0x30000000L
    X509* certificate = SSL_get1_peer_certificate(ssl);
#else
    X509* certificate = SSL_get_peer_certificate(ssl);
#endif
    if (!certificate) throw std::runtime_error("TLS peer did not present an identity certificate");
    std::unique_ptr<X509, decltype(&X509_free)> holder(certificate, X509_free);
    EVP_PKEY* key = X509_get_pubkey(certificate);
    if (!key) cryptoError("Reading TLS peer public key");
    PrivateKey keyHolder(key, EVP_PKEY_free);
    if (EVP_PKEY_base_id(keyHolder.get()) != EVP_PKEY_ED25519)
        throw std::runtime_error("TLS peer identity key must use Ed25519");
    return peerIdFromKey(keyHolder.get());
}
