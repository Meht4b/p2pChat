#include "PeerId.h"

#include <algorithm>
#include <cstring>
#include <random>

namespace
{
    constexpr char kHexDigits[] = "0123456789abcdef";

    int hexValue(char c)
    {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }
}

PeerId PeerId::generate()
{
    // std::random_device is backed by the operating system's secure generator on
    // MSVC, glibc and libc++. (Use a proper crypto library once keys are involved.)
    std::random_device device;
    Bytes bytes{};
    for (std::size_t i = 0; i < kSize; i += sizeof(std::random_device::result_type))
    {
        const auto value = device();
        std::memcpy(bytes.data() + i, &value, sizeof(value));
    }
    return PeerId(bytes);
}

std::optional<PeerId> PeerId::fromHex(std::string_view hex)
{
    if (hex.size() != kSize * 2)
        return std::nullopt;

    Bytes bytes{};
    for (std::size_t i = 0; i < kSize; ++i)
    {
        const int high = hexValue(hex[2 * i]);
        const int low = hexValue(hex[2 * i + 1]);
        if (high < 0 || low < 0)
            return std::nullopt;
        bytes[i] = static_cast<uint8_t>((high << 4) | low);
    }
    return PeerId(bytes);
}

std::optional<PeerId> PeerId::fromBytes(const void* data, std::size_t size)
{
    if (!data || size != kSize)
        return std::nullopt;

    Bytes bytes{};
    std::memcpy(bytes.data(), data, kSize);
    return PeerId(bytes);
}

std::string PeerId::toHex() const
{
    std::string out;
    out.reserve(kSize * 2);
    for (uint8_t byte : bytes_)
    {
        out.push_back(kHexDigits[byte >> 4]);
        out.push_back(kHexDigits[byte & 0x0f]);
    }
    return out;
}

std::string PeerId::shortHex() const { return toHex().substr(0, 8); }

bool PeerId::isZero() const
{
    return std::all_of(bytes_.begin(), bytes_.end(), [](uint8_t b) { return b == 0; });
}
