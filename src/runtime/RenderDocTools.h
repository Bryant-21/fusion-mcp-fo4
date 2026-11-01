#pragma once
// Project-owned GPL-3.0 code. fusionmcp.renderdoc.* control tools over RenderDoc's
// in-application API. Optional: everything reports unavailable unless
// renderdoc.dll is in the process (preloaded here, or by another plugin).
#include "core/ProviderRegistry.h"
#include <vector>

namespace fusionmcp::runtime {
// Must run at plugin load, before the D3D device exists: RenderDoc only hooks
// devices created after it loads. Off unless [RenderDoc] Load = true in FusionMCP.toml.
void PreloadRenderDoc();
std::vector<ProviderRegistry::ControlTool> RenderDocTools();
}
