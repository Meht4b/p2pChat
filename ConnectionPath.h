#pragma once

#include <asio/ip/address.hpp>
#include <cstdint>

enum class ConnectionPath : std::uint8_t { Unknown, Lan, Direct, NatTraversal, Relay };

inline bool isLanAddress(const asio::ip::address& address)
{
    if (address.is_loopback()) return true;
    if (address.is_v4()) {
        const auto bytes = address.to_v4().to_bytes();
        return bytes[0] == 10 ||
            (bytes[0] == 172 && bytes[1] >= 16 && bytes[1] <= 31) ||
            (bytes[0] == 192 && bytes[1] == 168) ||
            (bytes[0] == 169 && bytes[1] == 254);
    }
    if (address.to_v6().is_link_local()) return true;
    const auto bytes = address.to_v6().to_bytes();
    return (bytes[0] & 0xfe) == 0xfc;
}

inline const char* connectionPathLabel(ConnectionPath path)
{
    switch (path) {
    case ConnectionPath::Lan: return "LAN";
    case ConnectionPath::Direct: return "DIRECT";
    case ConnectionPath::NatTraversal: return "DIRECT / ICE";
    case ConnectionPath::Relay: return "RELAY / TURN";
    case ConnectionPath::Unknown:
    default: return "PATH UNKNOWN";
    }
}
