#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

// 256-bit random identifier of a peer. Generated once per installation and stored locally.
class PeerId
{
public:
    static constexpr std::size_t kSize = 32;
    using Bytes = std::array<uint8_t, kSize>;

    PeerId() = default;                                  // all zero = "no id"
    explicit PeerId(const Bytes& bytes) : bytes_(bytes) {}

    // fresh random identifier
    static PeerId generate();

    // 64 hex characters (either case) -> id
    static std::optional<PeerId> fromHex(std::string_view hex);

    // exactly kSize bytes -> id
    static std::optional<PeerId> fromBytes(const void* data, std::size_t size);

    const Bytes& bytes() const { return bytes_; }
    std::string toHex() const;          // 64 characters, used for storage
    std::string shortHex() const;       // first 8 characters, used for display
    bool isZero() const;

    bool operator==(const PeerId&) const = default;
    auto operator<=>(const PeerId&) const = default;

private:
    Bytes bytes_{};
};

// Longest username we send or accept, in bytes.
constexpr std::size_t kMaxUsernameBytes = 32;

// A peer as announced in the handshake.
struct PeerInfo
{
    PeerId id;
    std::string username;
};

namespace std
{
    template <>
    struct hash<PeerId>
    {
        std::size_t operator()(const PeerId& id) const noexcept
        {
            // ids are uniformly random, so any 8 bytes make a good hash
            std::size_t h = 0;
            for (std::size_t i = 0; i < sizeof(std::size_t); ++i)
                h = (h << 8) | id.bytes()[i];
            return h;
        }
    };
}
