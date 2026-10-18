#include "RoomProtocol.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <stdexcept>

namespace room_protocol {
namespace {

using CipherContext = std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)>;
constexpr std::size_t kNonceBytes = 12;
constexpr std::size_t kTagBytes = 16;
constexpr std::uint8_t kEnvelopeVersion = 1;

RoomId digest(const char* domain, const std::string& token)
{
    std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)> context(EVP_MD_CTX_new(), EVP_MD_CTX_free);
    if (!context || EVP_DigestInit_ex(context.get(), EVP_sha256(), nullptr) != 1 ||
        EVP_DigestUpdate(context.get(), domain, std::char_traits<char>::length(domain)) != 1 ||
        EVP_DigestUpdate(context.get(), token.data(), token.size()) != 1)
        throw std::runtime_error("Deriving room cryptographic values failed");
    RoomId result{};
    unsigned int size = 0;
    if (EVP_DigestFinal_ex(context.get(), result.data(), &size) != 1 || result.size() != size)
        throw std::runtime_error("Deriving room cryptographic values failed");
    return result;
}

} // namespace

std::vector<std::uint8_t> encodeFrame(Type type, const std::vector<std::uint8_t>& payload)
{
    const std::size_t body_size = payload.size() + 1;
    if (body_size > kMaximumFrameBytes)
        throw std::length_error("Rendezvous frame is too large");
    std::vector<std::uint8_t> result(4 + body_size);
    const auto length = static_cast<std::uint32_t>(body_size);
    result[0] = static_cast<std::uint8_t>(length >> 24);
    result[1] = static_cast<std::uint8_t>(length >> 16);
    result[2] = static_cast<std::uint8_t>(length >> 8);
    result[3] = static_cast<std::uint8_t>(length);
    result[4] = static_cast<std::uint8_t>(type);
    std::copy(payload.begin(), payload.end(), result.begin() + 5);
    return result;
}

bool decodeFrame(const std::vector<std::uint8_t>& bytes, Frame& frame)
{
    if (bytes.size() < 5) return false;
    const std::uint32_t length = (static_cast<std::uint32_t>(bytes[0]) << 24) |
        (static_cast<std::uint32_t>(bytes[1]) << 16) |
        (static_cast<std::uint32_t>(bytes[2]) << 8) | bytes[3];
    if (length == 0 || length > kMaximumFrameBytes || bytes.size() != 4u + length)
        return false;
    frame.type = static_cast<Type>(bytes[4]);
    frame.payload.assign(bytes.begin() + 5, bytes.end());
    return true;
}

RoomId roomIdForToken(const std::string& token)
{
    if (token.size() < 16)
        throw std::invalid_argument("Room token must contain at least 16 characters; use a private, hard-to-guess token");
    return digest("p2pChat rendezvous room id v1", token);
}

std::vector<std::uint8_t> encryptDescription(const std::string& token,
    const RoomId& room_id, const std::string& description)
{
    if (description.size() > kMaximumFrameBytes - 29)
        throw std::length_error("ICE description is too large for rendezvous signaling");
    const RoomId key = digest("p2pChat rendezvous description key v1", token);
    std::array<std::uint8_t, kNonceBytes> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1)
        throw std::runtime_error("Creating rendezvous nonce failed");

    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_EncryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
            static_cast<int>(nonce.size()), nullptr) != 1 ||
        EVP_EncryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce.data()) != 1)
        throw std::runtime_error("Initializing rendezvous encryption failed");

    int written = 0;
    if (EVP_EncryptUpdate(context.get(), nullptr, &written, room_id.data(),
            static_cast<int>(room_id.size())) != 1)
        throw std::runtime_error("Authenticating rendezvous room failed");
    std::vector<std::uint8_t> ciphertext(description.size() + 16);
    int ciphertext_size = 0;
    if (EVP_EncryptUpdate(context.get(), ciphertext.data(), &written,
            reinterpret_cast<const std::uint8_t*>(description.data()),
            static_cast<int>(description.size())) != 1)
        throw std::runtime_error("Encrypting rendezvous description failed");
    ciphertext_size += written;
    if (EVP_EncryptFinal_ex(context.get(), ciphertext.data() + ciphertext_size, &written) != 1)
        throw std::runtime_error("Encrypting rendezvous description failed");
    ciphertext_size += written;
    ciphertext.resize(static_cast<std::size_t>(ciphertext_size));
    std::array<std::uint8_t, kTagBytes> tag{};
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_GET_TAG,
            static_cast<int>(tag.size()), tag.data()) != 1)
        throw std::runtime_error("Finalizing rendezvous encryption failed");

    std::vector<std::uint8_t> envelope;
    envelope.reserve(1 + nonce.size() + ciphertext.size() + tag.size());
    envelope.push_back(kEnvelopeVersion);
    envelope.insert(envelope.end(), nonce.begin(), nonce.end());
    envelope.insert(envelope.end(), ciphertext.begin(), ciphertext.end());
    envelope.insert(envelope.end(), tag.begin(), tag.end());
    return envelope;
}

bool decryptDescription(const std::string& token, const RoomId& room_id,
    const std::vector<std::uint8_t>& encrypted, std::string& description)
{
    if (encrypted.size() < 1 + kNonceBytes + kTagBytes ||
        encrypted.size() > kMaximumFrameBytes - 1 || encrypted[0] != kEnvelopeVersion)
        return false;
    const RoomId key = digest("p2pChat rendezvous description key v1", token);
    const auto* nonce = encrypted.data() + 1;
    const auto* ciphertext = nonce + kNonceBytes;
    const std::size_t ciphertext_size = encrypted.size() - 1 - kNonceBytes - kTagBytes;
    const auto* tag = ciphertext + ciphertext_size;

    CipherContext context(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    if (!context || EVP_DecryptInit_ex(context.get(), EVP_aes_256_gcm(), nullptr, nullptr, nullptr) != 1 ||
        EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_IVLEN,
            static_cast<int>(kNonceBytes), nullptr) != 1 ||
        EVP_DecryptInit_ex(context.get(), nullptr, nullptr, key.data(), nonce) != 1)
        return false;
    int written = 0;
    if (EVP_DecryptUpdate(context.get(), nullptr, &written, room_id.data(),
            static_cast<int>(room_id.size())) != 1)
        return false;
    std::vector<std::uint8_t> plaintext(ciphertext_size + 16);
    int plaintext_size = 0;
    if (EVP_DecryptUpdate(context.get(), plaintext.data(), &written,
            ciphertext, static_cast<int>(ciphertext_size)) != 1)
        return false;
    plaintext_size += written;
    std::array<std::uint8_t, kTagBytes> tag_copy{};
    std::copy(tag, tag + tag_copy.size(), tag_copy.begin());
    if (EVP_CIPHER_CTX_ctrl(context.get(), EVP_CTRL_GCM_SET_TAG,
            static_cast<int>(tag_copy.size()), tag_copy.data()) != 1 ||
        EVP_DecryptFinal_ex(context.get(), plaintext.data() + plaintext_size, &written) != 1)
        return false;
    plaintext_size += written;
    description.assign(reinterpret_cast<const char*>(plaintext.data()),
        static_cast<std::size_t>(plaintext_size));
    return true;
}

} // namespace room_protocol
