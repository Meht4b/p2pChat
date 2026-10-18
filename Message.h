#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

#include "PeerId.h"

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

// ---- handshake -------------------------------------------------------------
// Body layout (the frame length already tells us where the username ends):
//
//   [ 32 bytes: peer id ][ username: UTF-8, 1..kMaxUsernameBytes bytes ]
//
// username size = body size - PeerId::kSize

// Throws std::invalid_argument if the username is empty or longer than kMaxUsernameBytes.
Message makeHandshake(const PeerInfo& info);

// Returns false if the body is malformed. The username is sanitized (control characters
// removed, whitespace replaced by '_') because it comes from an untrusted peer.
bool parseHandshake(const std::vector<char>& body, PeerInfo& out);
