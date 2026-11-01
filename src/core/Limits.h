#pragma once
#include <fusionmcp/ProviderAPI.h>
#include <cstddef>
namespace fusionmcp {
// Budgets include serialization and every host/protocol wrapper. A provider
// envelope stays <=256 KiB/depth32; status and MCP envelopes have explicit slack.
inline constexpr std::size_t kMaxCatalogBytes=1024*1024;
inline constexpr unsigned kMaxCatalogDepth=40;
inline constexpr std::size_t kMaxDescriptorBytes=128*1024;
inline constexpr std::size_t kMaxCoreEnvelopeBytes=FUSIONMCP_MAX_RESULT_BYTES+4096;
inline constexpr unsigned kMaxProviderResultDepth=32;
inline constexpr unsigned kMaxCoreEnvelopeDepth=36;
inline constexpr unsigned kMaxWireDepth=40;
}
