#include "TextSanitizer.h"

#include <cstdint>

#include "PeerId.h"

namespace
{
    bool isForbiddenCodePoint(uint32_t cp)
    {
        return (cp >= 0x80 && cp <= 0x9F)          // C1 controls (includes 8-bit CSI)
            || cp == 0x2028 || cp == 0x2029        // line / paragraph separator
            || (cp >= 0x202A && cp <= 0x202E)      // bidi embedding / override
            || (cp >= 0x2066 && cp <= 0x2069);     // bidi isolates
    }
}

bool isValidUsername(const std::string& name)
{
    if (name.empty() || name.size() > kMaxUsernameBytes)
        return false;

    for (unsigned char c : name)
    {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                             (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!allowed)
            return false;
    }
    return true;
}

std::string sanitizeText(const std::string& input)
{
    std::string out;
    out.reserve(input.size());

    const auto* p = reinterpret_cast<const unsigned char*>(input.data());
    const std::size_t n = input.size();
    std::size_t i = 0;

    while (i < n)
    {
        const unsigned char c = p[i];

        if (c < 0x80)
        {
            if (c == '\t') out.push_back(' ');
            else if (c >= 0x20 && c != 0x7f) out.push_back(static_cast<char>(c));
            ++i;
            continue;
        }

        std::size_t len = 0;
        uint32_t cp = 0;
        if (c >= 0xC2 && c <= 0xDF)      { len = 2; cp = c & 0x1F; }
        else if (c >= 0xE0 && c <= 0xEF) { len = 3; cp = c & 0x0F; }
        else if (c >= 0xF0 && c <= 0xF4) { len = 4; cp = c & 0x07; }
        else { out.push_back('?'); ++i; continue; }

        bool valid = i + len <= n;
        for (std::size_t k = 1; valid && k < len; ++k)
        {
            if ((p[i + k] & 0xC0) != 0x80) valid = false;
            else cp = (cp << 6) | (p[i + k] & 0x3F);
        }

        if (valid)
        {
            if (len == 3 && cp < 0x800) valid = false;                       // overlong
            if (len == 4 && (cp < 0x10000 || cp > 0x10FFFF)) valid = false;  // overlong / out of range
            if (cp >= 0xD800 && cp <= 0xDFFF) valid = false;                 // surrogates
        }

        if (!valid) { out.push_back('?'); ++i; continue; }

        if (!isForbiddenCodePoint(cp))
            out.append(input, i, len);
        i += len;
    }
    return out;
}
