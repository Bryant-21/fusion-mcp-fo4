#pragma once
// Project-owned GPL-3.0 code. D3D11 half of the native screenshot; DLL only.
namespace fusionmcp {
class FrameCapture;
}
namespace fusionmcp::runtime {
// Hooks Present (IDXGISwapChain vtable slot 8) on the swap chain the game holds
// in BSGraphics::RendererData::renderWindow[0] and serves capture's requests
// from it. Main thread. Returns false while the renderer has no swap chain yet;
// safe to call again from a later F4SE message. stage names the message for the log.
bool InstallPresentCapture(FrameCapture& capture,const char* stage);
}
