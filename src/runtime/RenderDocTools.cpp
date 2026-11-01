// Project-owned GPL-3.0 code; see RenderDocTools.h.
#include "runtime/RenderDocTools.h"
#include "runtime/ConfigFile.h"
#include <F4SE/F4SE.h>
#include <Windows.h>
#include "core/Json.h"
#include <renderdoc/renderdoc_app.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <filesystem>
#include <string>

namespace fusionmcp::runtime {
namespace {
constexpr char kDefaultDll[]="C:\\Program Files\\RenderDoc\\renderdoc.dll";
constexpr unsigned kMaxFrames=8;

// Another plugin may load renderdoc.dll after this one, so a miss is not cached.
RENDERDOC_API_1_6_0* Api() {
    static std::atomic<RENDERDOC_API_1_6_0*> cached{nullptr};
    if (auto* api=cached.load(std::memory_order_acquire)) return api;
    const auto module=GetModuleHandleW(L"renderdoc.dll");
    if (!module) return nullptr;
    const auto getApi=reinterpret_cast<pRENDERDOC_GetAPI>(GetProcAddress(module,"RENDERDOC_GetAPI"));
    RENDERDOC_API_1_6_0* api=nullptr;
    if (!getApi||getApi(eRENDERDOC_API_Version_1_6_0,reinterpret_cast<void**>(&api))!=1||!api) return nullptr;
    cached.store(api,std::memory_order_release);
    return api;
}

Json Unavailable() {
    return Error("unavailable","renderdoc.dll is not loaded. Set [RenderDoc] Load = true in FusionMCP.toml (or enable F4FS's "
        "[RenderDoc] bEnabled) and restart the game; it must load before the D3D device is created.");
}

struct CaptureRow {std::uint32_t index;std::string path;std::uint64_t timestamp;};
std::vector<CaptureRow> Captures(RENDERDOC_API_1_6_0& api) {
    std::vector<CaptureRow> rows;
    const auto count=api.GetNumCaptures();
    for (std::uint32_t i=0;i<count;++i) {
        std::uint32_t length=0;
        if (!api.GetCapture(i,nullptr,&length,nullptr)||!length) continue;
        std::string path(length,'\0');
        std::uint64_t timestamp=0;
        if (!api.GetCapture(i,path.data(),&length,&timestamp)) continue;
        path.resize(std::strlen(path.c_str()));
        rows.push_back({i,std::move(path),timestamp});
    }
    return rows;
}

Json Status() {
    auto* api=Api();
    if (!api) return Success({{"loaded",false},{"captures",0}});
    int major=0,minor=0,patch=0;
    api->GetAPIVersion(&major,&minor,&patch);
    const char* pathTemplate=api->GetCaptureFilePathTemplate();
    return Success({{"loaded",true},{"apiVersion",std::to_string(major)+"."+std::to_string(minor)+"."+std::to_string(patch)},
        {"pathTemplate",pathTemplate?pathTemplate:""},{"captures",api->GetNumCaptures()},
        {"capturing",api->IsFrameCapturing()==1},{"uiConnected",api->IsTargetControlConnected()==1}});
}

Json Capture(const Json& args) {
    auto* api=Api();
    if (!api) return Unavailable();
    // One capture at a time across triggers, including f4fs.capture action=renderdoc.
    if (api->IsFrameCapturing()==1) return Error("busy","A RenderDoc capture is already in progress");
    const auto frames=std::clamp(args.value("frames",1u),1u,kMaxFrames);
    const auto before=api->GetNumCaptures();
    if (frames==1) api->TriggerCapture();
    else api->TriggerMultiFrameCapture(frames);
    return Success({{"triggered",true},{"frames",frames},{"capturesBefore",before},
        {"note","The capture is written over the next frames. Poll fusionmcp.renderdoc.captures until count exceeds capturesBefore."}});
}

Json List(const Json& args) {
    auto* api=Api();
    if (!api) return Unavailable();
    auto rows=Captures(*api);
    std::sort(rows.begin(),rows.end(),[](const auto& a,const auto& b){return a.index>b.index;});
    const auto limit=args.value("limit",10u);
    Json list=Json::array();
    for (const auto& row:rows) {
        if (list.size()>=limit) break;
        std::error_code ec;
        const auto bytes=std::filesystem::file_size(std::filesystem::path(std::u8string(row.path.begin(),row.path.end())),ec);
        list.push_back({{"index",row.index},{"path",row.path},{"timestamp",row.timestamp},{"bytes",ec?0:bytes}});
    }
    return Success({{"count",rows.size()},{"captures",std::move(list)}});
}
}

void PreloadRenderDoc() {
    const auto file=LoadConfig();
    if (!file.GetBool("RenderDoc","Load",false)) return;
    if (GetModuleHandleW(L"renderdoc.dll")) {REX::INFO("FusionMCP RenderDoc already loaded by another module");return;}
    const auto text=file.GetString("RenderDoc","DllPath",kDefaultDll);
    const std::filesystem::path dll(std::u8string(text.begin(),text.end()));
    if (LoadLibraryW(dll.c_str())) REX::INFO("FusionMCP RenderDoc loaded");
    else REX::WARN("FusionMCP RenderDoc LoadLibrary failed (error {}); check [RenderDoc] DllPath",GetLastError());
}

std::vector<ProviderRegistry::ControlTool> RenderDocTools() {
    const auto schema=[](const char* text){return Json::parse(text);};
    return {
        {"fusionmcp.renderdoc.status","RenderDoc in-application API state: loaded, version, capture path template, capture count, "
            "whether a capture is in progress and whether the RenderDoc UI is connected.",
            schema(R"({"type":"object","properties":{},"additionalProperties":false})"),false,
            [](const Json&,bool){return Status();},"renderdoc"},
        {"fusionmcp.renderdoc.capture","Trigger a whole-frame RenderDoc capture of the next frame (or the next 'frames', at most 8). Returns at once; "
            "poll fusionmcp.renderdoc.captures for the written .rdc. Refused while a capture is in progress. For F4FS-scoped RTGI/RT-frame "
            "captures use f4fs.capture action=renderdoc. Needs renderdoc.dll loaded before the device ([RenderDoc] Load=1).",
            schema(R"({"type":"object","properties":{"frames":{"type":"integer","minimum":1,"maximum":8}},"additionalProperties":false})"),true,
            [](const Json& args,bool){return Capture(args);},"renderdoc"},
        {"fusionmcp.renderdoc.captures","Captures RenderDoc has written this session, newest first: index, .rdc path, timestamp, bytes. "
            "Lists every capture in the process, including those triggered by f4fs.capture action=renderdoc.",
            schema(R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":100}},"additionalProperties":false})"),false,
            [](const Json& args,bool){return List(args);},"renderdoc"},
    };
}
}
