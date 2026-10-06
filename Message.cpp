#include "Message.h"

#include <cstring>
#include <stdexcept>

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
