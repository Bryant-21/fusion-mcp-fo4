#pragma once
#include <fusionframework/Config.h>

namespace fusionmcp::runtime {
// FusionMCP.toml next to the DLL; missing or malformed reads as an empty document (all defaults).
fusionframework::config::Document LoadConfig();
}
