// Original project-owned standalone bootstrap. No F4FS/Tales/DevTools dependency.
#include <F4SE/F4SE.h>
#include <Windows.h>
#include "core/BatchCoordinator.h"
#include "core/BuiltinCommands.h"
#include "core/F4fsTools.h"
#include "core/ProviderRegistry.h"
#include "core/GameTaskDispatcher.h"
#include "core/OwnerWakeService.h"
#include "core/ShutdownJoinService.h"
#include "transport/Server.h"
#include "transport/PortOverride.h"
#include "runtime/ConfigFile.h"
#include "fusionframework/Log.h"
#include "fusionframework/Require.h"
#include "runtime/LegacyQuery.h"
#include "runtime/NativeGameServices.h"
#include "runtime/PresentCapture.h"
#include "runtime/RenderDocTools.h"
#include <array>
#include <filesystem>
#include <type_traits>
#include "core/IdleWatchdog.h"

namespace {
using namespace fusionmcp;
std::atomic_bool shutdownRequested{false};
ProviderRegistry& Core() {static auto* const core=new ProviderRegistry;return *core;}
// Process lifetime; set once in F4SEPlugin_Load before any message or wake.
runtime::NativeGameServices* nativeServices=nullptr;
BuiltinCommands* builtins=nullptr;
BatchCoordinator& Batches() {static auto* const batches=new BatchCoordinator(Core());return *batches;}
IdleWatchdog& Watchdog() {static auto* const watchdog=new IdleWatchdog;return *watchdog;}
// [MCP] IdleQuitMinutes in FusionMCP.toml next to the DLL; 0 (default) is off.
unsigned IdleQuitMinutes() {
    const auto minutes=runtime::LoadConfig().GetInt("MCP","IdleQuitMinutes",0);
    return static_cast<unsigned>(std::clamp<std::int64_t>(minutes,0,IdleWatchdog::kMaxMinutes));
}
void RegisterBuiltins(const REL::Version& version,bool editor) {
    const auto services=std::make_shared<runtime::NativeGameServices>(!editor&&runtime::ConsoleVerifiedRuntime(version),
        editor?-1:runtime::VerifiedRuntimeSlot(version));
    nativeServices=services.get();
    builtins=new BuiltinCommands(services);
    if (!builtins->RegisterWith(Core())||!Batches().RegisterWith(Core())) REX::WARN("FusionMCP built-in commands unavailable");
    builtins->SetF4fs([](const std::string& tool,const Json& args) {return CallF4fs(&ResolveLoadedF4fs,tool,args);});
    if (!Core().RegisterControl(Watchdog().Tool([](const std::string& line) {REX::INFO("{}",line);})))
        REX::WARN("FusionMCP idle watchdog tool unavailable");
    for (auto& tool:runtime::RenderDocTools()) if (!Core().RegisterControl(std::move(tool))) REX::WARN("FusionMCP RenderDoc tools unavailable");
    for (auto& tool:F4fsTools(&ResolveLoadedF4fs)) if (!Core().RegisterControl(std::move(tool))) REX::WARN("FusionMCP f4fs tools unavailable");
    REX::INFO("FusionMCP runtime {} native Game adapter {}",version.string(),services->ConsoleSupported()?"verified":"unavailable");
}
transport::Server& Server() {static auto* const server=new transport::Server;return *server;}
bool PostGameTask(GameTaskDispatcher::Task task) {
    const auto* tasks=F4SE::GetTaskInterface();
    if (!tasks||tasks->Version()!=F4SE::TaskInterface::kVersion) return false;
    tasks->AddTask([work=std::move(task)]() noexcept {try {work();} catch (...) {}});
    return true; // Submission is not an execution/completion acknowledgement.
}
GameTaskDispatcher& Game() {
    static auto* const game=new GameTaskDispatcher(&PostGameTask,[](EpochId epoch,std::size_t budget) {
        Core().Pump(Owner::Game,budget,epoch);
        if (builtins) builtins->SampleOnGame(epoch);
    });
    return *game;
}
void Wake() {
    if (Core().Pending(Owner::Game)||(builtins&&builtins->NeedsGameWake())) Game().RequestWake(Core().Epoch());
}
OwnerWakeService& Progress() {static auto* const progress=new OwnerWakeService;return *progress;}
ShutdownJoinService& Cleanup() {static auto* const cleanup=new ShutdownJoinService;return *cleanup;}
bool HasPending() {
    return Core().Pending(Owner::Game)||Core().Pending(Owner::Render)||Core().Pending(Owner::UI)||Core().Pending(Owner::VM)||
        (builtins&&builtins->NeedsGameWake())||Batches().Running();
}
void QuitIfIdle() {
    if (!Watchdog().Expired()) return;
    REX::WARN("FusionMCP idle watchdog: no MCP request for {} min; quitting the game cleanly",Watchdog().Status()["minutes"].get<unsigned>());
    if (!PostGameTask([] {
            std::string error;
            if (auto* world=nativeServices?nativeServices->World():nullptr;!world||!world->RequestQuit(error))
                REX::WARN("FusionMCP idle watchdog could not quit: {}",error.empty()?"no Game adapter":error);
        })) REX::WARN("FusionMCP idle watchdog could not reach the Game owner to quit");
}
void Tick() {Batches().Tick();QuitIfIdle();Wake();}
std::shared_ptr<const transport::Backend> Backend() {
    static const auto backend=std::make_shared<transport::Backend>(transport::Backend{
        [] {Watchdog().Touch();return Core().Describe().dump();},
        [](const std::string& command,const std::string& args,bool mutations) {
            Watchdog().Touch();
            auto result=Core().Invoke(command,args,mutations).dump();Wake();return result;
        }, &Tick, [](const std::string& name) {Watchdog().Touch();return Core().Lookup(name).dump();}});
    return backend;
}
transport::Config ReadConfig() {
    transport::Config config;
    std::array<wchar_t,2> disabledValue{};
    SetLastError(ERROR_SUCCESS);
    const auto disabledLength=GetEnvironmentVariableW(L"FUSIONMCP_DISABLED",disabledValue.data(),static_cast<DWORD>(disabledValue.size()));
    const auto disableOverride=(disabledLength||GetLastError()!=ERROR_ENVVAR_NOT_FOUND)
        ? std::optional<std::wstring_view>(disabledLength==1 ? std::wstring_view(disabledValue.data(),1) : std::wstring_view{}) : std::nullopt;
    if (transport::DisabledByLaunch(disableOverride)) {
        config.enabled=false;
        REX::INFO("FusionMCP disabled by launcher");
        return config;
    }
    const auto file=runtime::LoadConfig();
    config.enabled=file.GetBool("MCP","Enabled",false);
    config.allowMutations=file.GetBool("MCP","AllowMutations",false);
    const auto port=file.GetInt("MCP","Port",8922);
    config.port=port&&port<=65535 ? static_cast<uint16_t>(port) : 0;
    std::array<wchar_t, 6> overrideValue{};
    SetLastError(ERROR_SUCCESS);
    const auto overrideLength=GetEnvironmentVariableW(L"FUSIONMCP_PORT",overrideValue.data(),static_cast<DWORD>(overrideValue.size()));
    if (overrideLength || GetLastError()!=ERROR_ENVVAR_NOT_FOUND) {
        const auto overridePort=overrideLength<overrideValue.size()
            ? transport::ParsePortOverride(std::wstring_view(overrideValue.data(),overrideLength)) : std::nullopt;
        if (!overridePort) {
            REX::WARN("FUSIONMCP_PORT must be a decimal port from 1 to 65535; transport disabled");
            config.enabled=false;
            return config;
        }
        config.port=*overridePort;
    }
    if (!config.port) {config.enabled=false;return config;}
    REX::INFO("FusionMCP configured loopback port {}",config.port);
    return config;
}
// The renderer's swap chain exists from some point during startup; the first
// message that finds it installs the hook (the log names which one).
void TryInstallCapture(const char* stage) {
    if (nativeServices&&nativeServices->Frames()) runtime::InstallPresentCapture(*nativeServices->Frames(),stage);
}
void Message(F4SE::MessagingInterface::Message* message) noexcept {
    try {
        if (!message) return;
        switch(message->type) {
        case F4SE::MessagingInterface::kInputLoaded: TryInstallCapture("kInputLoaded");break;
        case F4SE::MessagingInterface::kPostPostLoad: {
            TryInstallCapture("kPostPostLoad");
            if (shutdownRequested.load(std::memory_order_acquire)) break;
            auto config=ReadConfig();Core().SetMutationsAllowed(config.enabled&&config.allowMutations);
            if (!config.enabled) {REX::INFO("FusionMCP transport disabled");break;}
            if (const auto minutes=IdleQuitMinutes()) {
                Watchdog().Configure(minutes);
                REX::INFO("FusionMCP idle watchdog armed from the INI: quit after {} min without an MCP request",minutes);
            }
            if (nativeServices) nativeServices->InstallConsoleHooks();
            if (!Cleanup().Start([]{return Server().Stop();})) {Core().SetMutationsAllowed(false);REX::WARN("FusionMCP cleanup worker unavailable");break;}
            if (!Progress().Start(&Wake,&HasPending)) {
                Core().SetMutationsAllowed(false);Server().RequestStop();Cleanup().Request();
                REX::WARN("FusionMCP owner wake service unavailable");break;
            }
            if (!Server().Start(std::move(config),&Backend)) {
                Core().SetMutationsAllowed(false);Progress().BeginDrain();Cleanup().Request();
                REX::WARN("FusionMCP could not start configured loopback transport");
            }
            break;
        }
        case F4SE::MessagingInterface::kGameDataReady: {
            TryInstallCapture("kGameDataReady");
            if (nativeServices&&nativeServices->Frames()&&!nativeServices->Frames()->Installed())
                REX::WARN("FusionMCP native capture unavailable: the renderer has no swap chain at kGameDataReady");
            const auto* tasks=F4SE::GetTaskInterface();
            const auto available=tasks&&tasks->Version()==F4SE::TaskInterface::kVersion;
            const auto epoch=Core().AdvanceEpoch();Core().SetOwnerAvailable(Owner::Game,available);
            Game().SetLifecycle(epoch,available);
            if (nativeServices) {nativeServices->ResolveSaveDirectory();REX::INFO("FusionMCP save directory: {}",nativeServices->SaveDirectory());}
            break;
        }
        case F4SE::MessagingInterface::kPreLoadGame: {
            const auto epoch=Core().AdvanceEpoch();Game().SetLifecycle(epoch,false);
            // F4SE supplies the save file path; the transaction binds only its own target.
            const auto* text=static_cast<const char*>(message->data);
            const std::string path=text?std::string(text,strnlen(text,message->dataLen?std::min<std::size_t>(message->dataLen,4096):4096)):std::string();
            REX::INFO("FusionMCP PreLoadGame dataLen={} path={}",message->dataLen,path);
            if (builtins) builtins->Saves().OnPreLoad(path);
            break;
        }
        case F4SE::MessagingInterface::kPostLoadGame: {
            // Owner scheduling resumes. The success flag only advances a matching
            // save transaction; readiness still needs stable owner samples.
            const auto epoch=Core().AdvanceEpoch();Game().SetLifecycle(epoch,true);
            const bool success=message->data!=nullptr;
            REX::INFO("FusionMCP PostLoadGame dataLen={} data={} success={}",message->dataLen,reinterpret_cast<std::uintptr_t>(message->data),success);
            if (builtins) builtins->Saves().OnPostLoad(success,epoch);
            Wake();break;
        }
        case F4SE::MessagingInterface::kNewGame: {
            if (builtins) builtins->Saves().OnNewGame();
            break;
        }
        default:break;
        }
    } catch (...) {}
}
FUSIONMCP_Status FUSIONMCP_CALL Register(const FUSIONMCP_ProviderV1* provider,FUSIONMCP_ProviderHandle* handle) noexcept {
    try {return Core().Register(provider,handle);} catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
}
FUSIONMCP_Status FUSIONMCP_CALL Enable(FUSIONMCP_ProviderHandle handle,uint32_t enabled) noexcept {
    try {if (enabled>1) return FUSIONMCP_INVALID_ARGUMENT;const auto result=Core().SetEnabled(handle,enabled==1);Wake();return result;}
    catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
}
FUSIONMCP_Status FUSIONMCP_CALL Unregister(FUSIONMCP_ProviderHandle handle) noexcept {
    try {const auto result=Core().Unregister(handle);Wake();return result;} catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
}
const FUSIONMCP_HostV1 host{FUSIONMCP_ABI_V1,sizeof(FUSIONMCP_HostV1),&Register,&Enable,&Unregister};
static_assert(std::is_standard_layout_v<FUSIONMCP_HostV1>);
static_assert(std::is_standard_layout_v<FUSIONMCP_ProviderV1>);
}

extern "C" __declspec(dllexport) const FUSIONMCP_HostV1* FUSIONMCP_CALL
FUSIONMCP_GetHostAPI(uint32_t version,uint32_t size) noexcept {
    return version==FUSIONMCP_ABI_V1&&size==sizeof(FUSIONMCP_HostV1)?&host:nullptr;
}
F4SE_PLUGIN_QUERY(const F4SE::QueryInterface* f4se,F4SE::PluginInfo* info) {
    if (!f4se||!info) return false;
    const auto* version=F4SE::PluginVersionData::GetSingleton();
    if (!version) return false;
    info->infoVersion=F4SE::PluginInfo::kVersion;info->name=version->GetPluginName().data();info->version=version->pluginVersion;
    return transport::SupportsLegacyQuery(f4se->RuntimeVersion().pack(),f4se->IsEditor());
}
F4SE_PLUGIN_LOAD(const F4SE::LoadInterface* f4se) {
    try {
        // Trampoline: three 5-byte console call patches (runtime/ConsoleHooks.cpp).
        F4SE::Init(f4se,{.log=false,.trampoline=true,.trampolineSize=64,.hook=false});
        fusionframework::log::Init("FusionMCP");
        // Nothing else is registered when FusionFramework.dll is missing or too old.
        if (!fusionframework::require::Check("Fusion MCP",{fusionframework::require::kFramework}).empty()) return true;
        if (!f4se->IsEditor()) runtime::PreloadRenderDoc();
        RegisterBuiltins(f4se->RuntimeVersion(),f4se->IsEditor());
        const auto* messaging=F4SE::GetMessagingInterface();return messaging&&messaging->RegisterListener(&Message);
    } catch (...) {return false;}
}
// Nonblocking shutdown initiation; true does not mean the listener has joined or
// provider leases retired. The dedicated cleanup worker joins off engine owners.
// Outside DllMain only. This never authorizes unloading this or provider DLLs.
extern "C" __declspec(dllexport) bool FUSIONMCP_Shutdown() noexcept {
    try {
        shutdownRequested.store(true,std::memory_order_release);
        Server().RequestStop();Core().Stop();Cleanup().Request();
        const auto* tasks=F4SE::GetTaskInterface();
        Game().SetLifecycle(Core().Epoch(),tasks&&tasks->Version()==F4SE::TaskInterface::kVersion);
        Progress().BeginDrain();Wake();return true;
    } catch (...) {return false;}
}
