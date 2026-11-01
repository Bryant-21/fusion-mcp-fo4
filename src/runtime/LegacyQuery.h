#pragma once

#include <cstdint>

namespace fusionmcp::transport
{
    // F4SE's packed version format: major/minor/build/subversion.
    inline constexpr std::uint32_t kLegacyRuntime =
        (1u << 24) | (10u << 16) | (163u << 4);

    // NG/AE use the version export; the classic Query admits only OG 1.10.163.
    constexpr bool SupportsLegacyQuery(std::uint32_t runtime, bool isEditor) noexcept
    {
        return !isEditor && runtime == kLegacyRuntime;
    }
}
