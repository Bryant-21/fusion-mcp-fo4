#pragma once
#include <cstdint>
#include <optional>
#include <string_view>

namespace fusionmcp::transport
{
    inline bool DisabledByLaunch(std::optional<std::wstring_view> value)
    {
        return value.has_value() && *value != L"0";
    }

    inline std::optional<std::uint16_t> ParsePortOverride(std::wstring_view value)
    {
        if (value.empty() || value.size() > 5) return std::nullopt;
        unsigned port = 0;
        for (const auto digit : value) {
            if (digit < L'0' || digit > L'9') return std::nullopt;
            port = port * 10 + static_cast<unsigned>(digit - L'0');
        }
        if (!port || port > 65535) return std::nullopt;
        return static_cast<std::uint16_t>(port);
    }
}
