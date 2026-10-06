#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

constexpr std::size_t MAX_MESSAGE_SIZE = 64 * 1024;

enum class MessageType : uint8_t
{
	Handshake,
	Chat
};

//structure for message 
struct Message {
	MessageType type;
	std::vector<char> body;
};

std::vector<char> serialize(const Message& msg);

Message deserialize(const std::vector<char>& data);
