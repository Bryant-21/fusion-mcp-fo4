// Project-owned GPL-3.0 code; see F4fsTools.h.
#include "core/F4fsTools.h"
#include <Windows.h>
#include <atomic>

namespace fusionmcp {
namespace {
namespace api=f4fs::remote::api;

struct Sink {
    std::string bytes;
    unsigned calls=0;
    bool failed=false;
    static bool Write(void* opaque,const char* data,std::uint32_t size) noexcept {
        auto& self=*static_cast<Sink*>(opaque);
        try {
            if (++self.calls!=1||!data||size>api::kMaxResultBytes) {self.failed=true;return false;}
            self.bytes.assign(data,size);return true;
        } catch (...) {self.failed=true;return false;}
    }
};

Json Call(const api::ProviderV1& provider,const std::string& tool,const Json& args) {
    const auto serialized=args.dump();
    if (serialized.size()>api::kMaxArgumentsBytes) return Error("invalid_arguments","Argument size limit");
    Sink sink;
    const auto status=provider.invoke(tool.data(),static_cast<std::uint32_t>(tool.size()),
        serialized.data(),static_cast<std::uint32_t>(serialized.size()),&Sink::Write,&sink);
    if (status!=api::Result::Written||sink.failed||sink.calls!=1)
        return Error("unavailable","F4FS provider could not return a bounded result");
    try {
        auto result=ParseBounded(sink.bytes,api::kMaxResultBytes,32);
        return Envelope(result)?result:Error("provider_error","F4FS provider returned an invalid envelope");
    } catch (...) {return Error("provider_error","F4FS provider returned malformed JSON");}
}

std::string Reason(const F4fsResolver& resolve) {
    const auto resolved=resolve();
    if (!resolved.provider) return resolved.reason.empty()?"F4FS provider unavailable":resolved.reason;
    // The provider answers every call with "disabled" until F4FS.toml enables it.
    const auto status=Call(*resolved.provider,"f4fs.inspect",{{"kind","status"}});
    if (!status["ok"].get<bool>()&&status["error"]["code"]=="disabled")
        return "F4FS remote control is disabled: set [RemoteControl] bEnabled = true in F4FS.toml";
    return {};
}

struct Family {const char* name;const char* description;const char* schema;std::vector<std::string> mutatingActions;};
// Schemas follow F4FS src/RemoteControl/ToolDescriptors.cpp. Keys, values and
// feature ids stay free-form here: F4FS validates them against its own settings
// registry (unknown_key, invalid_value, read_only, forced). An older F4FS build
// refuses the actions it does not have.
const Family kFamilies[]={
    {"f4fs.feature","F4FS features. list/get: render-owner module metadata (copied snapshot). menu: the END-menu feature cards "
        "{id, name, page, on, forced, replaced, forcedBy, masters, inOffPreset}. set {id, enabled, persist?}: switch a menu card exactly "
        "as its switch does (masters, remembered values, preset follow-up); fails with forced when the GI mode or Advanced Lighting "
        "owns it and names the owner key.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["list","get","menu","set"],"default":"list"},"id":{"type":"string","maxLength":128},"enabled":{"type":"boolean"},"persist":{"type":"boolean"}},"additionalProperties":false})",
        {"set"}},
    {"f4fs.inspect","F4FS provider status, copied frame state, polling of an F4FS request id returned by a queued f4fs.* action "
        "(kind=request, requestId), or GPU zone timings (kind=perf, frames 0..240: the [Overlay] iMode=1 ring; available=false with the "
        "reason when timing is off). Cancellation only stops queued work; a started command may have applied.",
        R"({"type":"object","properties":{"kind":{"type":"string","enum":["status","state","request","perf"],"default":"status"},"action":{"type":"string","enum":["get","cancel"],"default":"get"},"requestId":{"type":"integer","minimum":1},"frames":{"type":"integer","minimum":0,"maximum":240}},"additionalProperties":false})",
        {"cancel"}},
    {"f4fs.shadercache","F4FS shader replacement statistics (copied snapshot). Cache deletion and compilation are unsupported.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["inspect"],"default":"inspect"}},"additionalProperties":false})",{}},
    {"f4fs.capture","F4FS capture owners. status/list read; queue runs an F4FS capture set; renderdoc asks F4FS's RenderDoc owner for a "
        "scoped RTGI/RT-frame capture (D3D12 interop aware). Whole-frame RenderDoc captures, status and the written .rdc list are fusionmcp.renderdoc.*. "
        "A queued result means the owner accepted it, not that an artifact exists; poll f4fs.inspect kind=request.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["status","list","queue","renderdoc"],"default":"status"},"set":{"type":"string","enum":["OpenPBR","PostTonemap","Skylighting","SSGI","SSGI-Instrument","Reflection","WorldProbes","TransportBake","ScreenSpaceShadows","FarFieldShadows"]},"frames":{"type":"integer","minimum":1,"maximum":120,"default":1},"scope":{"type":"string","enum":["rtgi","rtframe"],"default":"rtgi"}},"additionalProperties":false})",
        {"queue","renderdoc"}},
    {"f4fs.settings","F4FS INI settings as the END menu edits them. list {section?, filter?, offset?, limit?} returns each key's type, "
        "range or choices, current and default value and applies (live = next frame, session_load, restart with restartReason). "
        "get {key: Section.sKey}. set {key, value, persist?} validates like the menu, applies dependency forcing (reported in forced) "
        "and returns previous/value/effective/requiresRestart; not persisted unless persist. revert restores every key set this "
        "session. save persists the current INI. Errors: unknown_key, invalid_value, read_only, forced (owned by the GI mode or "
        "Advanced Lighting; the message names the owner key). Older F4FS builds only know Frame.fGameFrameCap and HighFps.bInstrument.",
        R"({"type":"object","properties":{"action":{"type":"string","enum":["list","get","set","revert","revert_all","save"],"default":"get"},"key":{"type":"string","minLength":1,"maxLength":128},"value":{"oneOf":[{"type":"number"},{"type":"boolean"},{"type":"string","maxLength":256}]},"persist":{"type":"boolean"},"section":{"type":"string","maxLength":64},"filter":{"type":"string","maxLength":128},"offset":{"type":"integer","minimum":0},"limit":{"type":"integer","minimum":1,"maximum":500}},"additionalProperties":false})",
        {"set","revert","revert_all","save"}},
};
}

F4fsResolution ResolveLoadedF4fs() {
    static std::atomic<const api::ProviderV1*> cached{nullptr};
    if (const auto* provider=cached.load(std::memory_order_acquire)) return {provider,{}};
    const auto module=GetModuleHandleA(api::kProviderModule);
    if (!module) return {nullptr,"F4FS.dll is not loaded"};
    const auto get=reinterpret_cast<api::GetProviderFn>(GetProcAddress(module,api::kProviderExport));
    if (!get) return {nullptr,"F4FS.dll has no F4FS_RemoteControl_GetAPI export; it predates the remote-control API"};
    const auto* provider=get(api::kVersion,sizeof(api::ProviderV1));
    if (!provider||provider->version!=api::kVersion||provider->size!=sizeof(api::ProviderV1)||!provider->describe||!provider->invoke)
        return {nullptr,"F4FS RemoteControl API version mismatch: FusionMCP requires v"+std::to_string(api::kVersion)};
    cached.store(provider,std::memory_order_release);
    return {provider,{}};
}

Json CallF4fs(const F4fsResolver& resolve,const std::string& tool,const Json& args) {
    const auto resolved=resolve();
    if (!resolved.provider) return Error("unavailable",resolved.reason);
    const auto reason=Reason(resolve);
    return reason.empty()?Call(*resolved.provider,tool,args):Error("unavailable",reason);
}
std::vector<ProviderRegistry::ControlTool> F4fsTools(F4fsResolver resolve) {
    std::vector<ProviderRegistry::ControlTool> tools;
    for (const auto& family:kFamilies) {
        const std::string name=family.name;
        tools.push_back({name,family.description,Json::parse(family.schema),false,
            [resolve,name](const Json& args,bool) {
                const auto resolved=resolve();
                return resolved.provider?Call(*resolved.provider,name,args):Error("unavailable",resolved.reason);
            },"f4fs",family.mutatingActions,[resolve] {return Reason(resolve);}});
    }
    return tools;
}
}
