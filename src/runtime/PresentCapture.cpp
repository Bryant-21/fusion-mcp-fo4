// Project-owned GPL-3.0 code. Present-time frame readback for fusionmcp.capture.screenshot.
//
// Seam: the Present slot of the swap chain object the game itself holds
// (RendererData::renderWindow[0].swapChain). F4SE offers no present callback and
// the engine's own present call site would need per-runtime ids, while this
// pointer is read through CommonLibF4 ids verified for 1.10.163, 1.11.221 and
// 1.11.240. The pixels come from that chain's GetBuffer(0) before the call is
// passed on, so they are the finished game frame including the Scaleform HUD:
//   - vanilla: the DXGI backbuffer (FO4 uses DXGI_SWAP_EFFECT_DISCARD, buffer 0);
//   - F4FS proxy presenter (native D3D11 or D3D12 frame generation): the game
//     holds F4FS's DXGISwapChainProxy, whose GetBuffer(0) is the fake backbuffer
//     the game drew into. F4FS draws its own menu and hands the frame to its
//     presenter inside the proxy's Present, i.e. after this capture.
//   - F4FS "native presentation" (a flip-model or fullscreen chain, e.g. with
//     HighFPSPhysicsFix): the game holds the real DXGI chain and F4FS draws its
//     overlay before Present, so the F4FS perf overlay/menu IS in the capture
//     (seen on Theseus 2026-10-09).
// Other overlays that patch the same slot later run before us (their drawing is
// included); ones that patched it earlier run after us (not included).
//
// Technique as in F4FS's Screenshot tool (see THIRD_PARTY.md): one
// CopySubresourceRegion into a staging texture, Map with DO_NOT_WAIT on later
// presents (blocking only after kNonBlockingMapFrames), rows handed to
// FrameCapture, which encodes the PNG on its worker thread.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include <Windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <wrl/client.h>
#include "runtime/PresentCapture.h"
#include "core/FrameCapture.h"
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace fusionmcp::runtime {
namespace {
using Microsoft::WRL::ComPtr;
using PresentFn=HRESULT(STDMETHODCALLTYPE*)(IDXGISwapChain*,UINT,UINT);
constexpr std::size_t kPresentSlot=8;
constexpr int kNonBlockingMapFrames=8;

std::atomic<PresentFn> originalPresent{nullptr};
std::atomic<FrameCapture*> target{nullptr};

// Render thread only.
struct Readback {
    std::uint64_t id=0;
    ComPtr<ID3D11Texture2D> staging;
    ComPtr<ID3D11DeviceContext> context;
    std::uint32_t width=0,height=0,format=0;
    PixelLayout layout=PixelLayout::Unsupported;
    int framesWaited=0;
};
Readback pending;

std::string Hex(HRESULT hr) {
    char text[16]{};
    std::snprintf(text,sizeof(text),"0x%08X",static_cast<unsigned>(hr));
    return text;
}
IDXGISwapChain* GameSwapChain() {
    auto* renderer=RE::BSGraphics::GetRendererData();
    return renderer?reinterpret_cast<IDXGISwapChain*>(renderer->renderWindow[0].swapChain):nullptr;
}
std::string ModuleOf(const void* address) {
    HMODULE module{};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
            static_cast<LPCWSTR>(address),&module)) return "unknown module";
    std::array<wchar_t,MAX_PATH> path{};
    if (!GetModuleFileNameW(module,path.data(),static_cast<DWORD>(path.size()))) return "unknown module";
    return std::filesystem::path(path.data()).filename().string();
}

void BeginCopy(FrameCapture& capture,IDXGISwapChain* chain,std::uint64_t id) {
    pending={};
    ComPtr<ID3D11Texture2D> source;
    if (const auto hr=chain->GetBuffer(0,IID_PPV_ARGS(&source));FAILED(hr)||!source) {
        capture.Fail(id,"capture_failed","The swap chain refused GetBuffer(0) (hr="+Hex(hr)+")");
        return;
    }
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    const auto layout=ClassifyFormat(desc.Format);
    if (layout==PixelLayout::Unsupported||desc.SampleDesc.Count!=1) {
        capture.Fail(id,"unsupported_format","Swap chain format "+std::to_string(desc.Format)+" with "+
            std::to_string(desc.SampleDesc.Count)+" sample(s) cannot be saved as PNG");
        return;
    }
    ComPtr<ID3D11Device> device;
    source->GetDevice(&device);
    ComPtr<ID3D11DeviceContext> context;
    if (device) device->GetImmediateContext(&context);
    D3D11_TEXTURE2D_DESC stagingDesc=desc;
    stagingDesc.MipLevels=1;stagingDesc.ArraySize=1;
    stagingDesc.Usage=D3D11_USAGE_STAGING;stagingDesc.BindFlags=0;
    stagingDesc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;stagingDesc.MiscFlags=0;
    ComPtr<ID3D11Texture2D> staging;
    if (!context||FAILED(device->CreateTexture2D(&stagingDesc,nullptr,&staging))) {
        capture.Fail(id,"capture_failed","Could not create a staging texture for the readback");
        return;
    }
    context->CopySubresourceRegion(staging.Get(),0,0,0,0,source.Get(),0,nullptr);
    pending={id,std::move(staging),std::move(context),desc.Width,desc.Height,static_cast<std::uint32_t>(desc.Format),layout,0};
}

void TryFinishReadback(FrameCapture& capture) {
    const UINT flags=pending.framesWaited<kNonBlockingMapFrames?D3D11_MAP_FLAG_DO_NOT_WAIT:0;
    D3D11_MAPPED_SUBRESOURCE mapped{};
    const auto hr=pending.context->Map(pending.staging.Get(),0,D3D11_MAP_READ,flags,&mapped);
    if (hr==DXGI_ERROR_WAS_STILL_DRAWING) {++pending.framesWaited;return;}
    const auto id=pending.id;
    if (FAILED(hr)||!mapped.pData) {
        pending={};
        capture.Fail(id,"capture_failed","Could not read the copied frame back (hr="+Hex(hr)+")");
        return;
    }
    RawFrame frame{pending.width,pending.height,pending.layout,pending.format,{}};
    const std::size_t rowBytes=std::size_t(frame.width)*4;
    frame.texels.resize(rowBytes*frame.height);
    const auto* source=static_cast<const std::uint8_t*>(mapped.pData);
    for (std::uint32_t y=0;y<frame.height;++y) std::memcpy(frame.texels.data()+rowBytes*y,source+std::size_t(mapped.RowPitch)*y,rowBytes);
    pending.context->Unmap(pending.staging.Get(),0);
    pending={};
    capture.Deliver(id,std::move(frame));
}

HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain* chain,UINT sync,UINT flags) {
    // The slot is shared by every chain of this class; only the game's chain is captured.
    if (auto* capture=target.load(std::memory_order_acquire);capture&&chain==GameSwapChain()) {
        std::uint64_t failing=0;
        try {
            if (pending.staging) {failing=pending.id;TryFinishReadback(*capture);}
            if (const auto id=capture->OnPresent()) {failing=*id;BeginCopy(*capture,chain,*id);}
        } catch (...) {
            pending={};
            if (failing) capture->Fail(failing,"capture_failed","Frame readback raised an exception");
        }
    }
    return originalPresent.load(std::memory_order_acquire)(chain,sync,flags);
}
}

bool InstallPresentCapture(FrameCapture& capture,const char* stage) {
    if (originalPresent.load(std::memory_order_acquire)) return true;
    auto* chain=GameSwapChain();
    if (!chain) return false;
    target.store(&capture,std::memory_order_release);
    auto** vtable=*reinterpret_cast<void***>(chain);
    void* current=vtable[kPresentSlot];
    // Published before the slot changes, so a Present racing the swap never calls null.
    originalPresent.store(reinterpret_cast<PresentFn>(current),std::memory_order_release);
    DWORD protect{};
    if (!VirtualProtect(&vtable[kPresentSlot],sizeof(void*),PAGE_EXECUTE_READWRITE,&protect)) {
        originalPresent.store(nullptr);
        capture.SetInstalled(false,"the swap chain vtable could not be made writable");
        return false;
    }
    void* previous=InterlockedCompareExchangePointer(&vtable[kPresentSlot],reinterpret_cast<void*>(&HookedPresent),current);
    VirtualProtect(&vtable[kPresentSlot],sizeof(void*),protect,&protect);
    if (previous!=current) {originalPresent.store(nullptr);return false;}
    const auto detail="IDXGISwapChain::Present (vtable slot 8) of the game's swap chain, chained to "+ModuleOf(current)+
        ", installed at "+stage;
    capture.SetInstalled(true,detail);
    REX::INFO("FusionMCP native capture: {}; screenshots go to {}",detail,capture.Directory().string());
    return true;
}
}
