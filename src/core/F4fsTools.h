#pragma once
// Project-owned GPL-3.0 code. f4fs.* tools routed to F4FS.dll's versioned
// RemoteControlAPI export. F4FS stays optional: without it the tools are
// listed with fusionmcp.available=false and a reason, and calls return unavailable.
#include "core/ProviderRegistry.h"
#include <f4fs/RemoteControlAPI.h>
#include <functional>
#include <string>
#include <vector>

namespace fusionmcp {
struct F4fsResolution {
    const f4fs::remote::api::ProviderV1* provider=nullptr;
    std::string reason; // Set when provider is null.
};
using F4fsResolver=std::function<F4fsResolution()>;

// Resolves the already-loaded F4FS.dll; never loads a library. Success is
// cached for the process lifetime (hot unloading is unsupported by both DLLs).
F4fsResolution ResolveLoadedF4fs();

// F4FS's invoke only reads copied snapshots and queues render-owner work, so
// the handlers are safe on the transport thread.
std::vector<ProviderRegistry::ControlTool> F4fsTools(F4fsResolver resolve);
// One f4fs.* call as the tools make it (no host schema check). Not for engine owners.
Json CallF4fs(const F4fsResolver& resolve,const std::string& tool,const Json& args);
}
