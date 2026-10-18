#include "Message.h"

#include <algorithm>
#include <cstring>
#include <stdexcept>

#include "TextSanitizer.h"

namespace {
bool validType(MessageType type)
{
    return type == MessageType::Handshake || type == MessageType::Chat;
}
}

std::vector<char> serialize(const Message& msg)
{
    if (!validType(msg.type)) throw std::runtime_error("Unknown message type");
    if (msg.body.size() + 1 > MAX_MESSAGE_SIZE) throw std::runtime_error("Message exceeds maximum size");
    const uint32_t length = static_cast<uint32_t>(msg.body.size() + 1);
    std::vector<char> data(4 + length);
    data[0] = static_cast<char>((length >> 24) & 0xff);
    data[1] = static_cast<char>((length >> 16) & 0xff);
    data[2] = static_cast<char>((length >> 8) & 0xff);
    data[3] = static_cast<char>(length & 0xff);
    data[4] = static_cast<char>(msg.type);
    if (!msg.body.empty()) std::memcpy(data.data() + 5, msg.body.data(), msg.body.size());
    return data;
}

Message deserialize(const std::vector<char>& data)
{
    if (data.size() < 5) throw std::runtime_error("Invalid message");
    const uint32_t length =
        (static_cast<uint32_t>(static_cast<unsigned char>(data[0])) << 24) |
        (static_cast<uint32_t>(static_cast<unsigned char>(data[1])) << 16) |
        (static_cast<uint32_t>(static_cast<unsigned char>(data[2])) << 8) |
         static_cast<uint32_t>(static_cast<unsigned char>(data[3]));
    if (length < 1 || length > MAX_MESSAGE_SIZE || data.size() != 4u + length)
        throw std::runtime_error("Invalid message length");
    const auto type = static_cast<MessageType>(static_cast<uint8_t>(data[4]));
    if (!validType(type)) throw std::runtime_error("Unknown message type");
    return {type, std::vector<char>(data.begin() + 5, data.end())};
}

Message makeHandshake(const PeerInfo& info)
{
    if (info.username.empty() || info.username.size() > kMaxUsernameBytes)
        throw std::invalid_argument("Username must be 1.." + std::to_string(kMaxUsernameBytes) + " bytes");

    std::vector<char> body;
    body.reserve(PeerId::kSize + info.username.size());
    body.insert(body.end(), info.id.bytes().begin(), info.id.bytes().end());
    body.insert(body.end(), info.username.begin(), info.username.end());
    return { MessageType::Handshake, std::move(body) };
}

bool parseHandshake(const std::vector<char>& body, PeerInfo& out)
{
    if (body.size() <= PeerId::kSize || body.size() > PeerId::kSize + kMaxUsernameBytes)
        return false;

    const auto id = PeerId::fromBytes(body.data(), PeerId::kSize);
    if (!id || id->isZero())
        return false;

    std::string name = sanitizeText(std::string(body.begin() + PeerId::kSize, body.end()));
    const auto first = name.find_first_not_of(' ');
    if (first == std::string::npos)
        return false;                                   // empty after cleaning
    name = name.substr(first, name.find_last_not_of(' ') - first + 1);
    std::replace(name.begin(), name.end(), ' ', '_');   // keeps names single-token for /selectpeer

    out.id = *id;
    out.username = std::move(name);
    return true;
}
