#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace room_protocol {

constexpr std::uint32_t kMaximumFrameBytes = 8192;
constexpr std::size_t kRoomIdBytes = 32;
using RoomId = std::array<std::uint8_t, kRoomIdBytes>;

enum class Type : std::uint8_t {
    Join = 1,
    Description = 2,
    Welcome = 0x81,
    PeerDescription = 0x82,
    Error = 0xff
};

struct Frame {
    Type type{};
    std::vector<std::uint8_t> payload;
};

struct TurnCredentials {
    std::uint16_t port = 0;
    std::string username;
    std::string password;
};

std::vector<std::uint8_t> encodeFrame(Type type, const std::vector<std::uint8_t>& payload);
bool decodeFrame(const std::vector<std::uint8_t>& bytes, Frame& frame);
RoomId roomIdForToken(const std::string& token);
std::vector<std::uint8_t> encryptDescription(const std::string& token,
    const RoomId& room_id, const std::string& description);
bool decryptDescription(const std::string& token, const RoomId& room_id,
    const std::vector<std::uint8_t>& encrypted, std::string& description);

} // namespace room_protocol
