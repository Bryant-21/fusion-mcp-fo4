// Original project-owned Game owner adapter over CommonLibF4. Every method runs
// on the F4SE game task owner; engine state is copied into DTOs before return.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include "runtime/NativeGameServices.h"
#include "core/ConsoleCatalog.h"
#include "runtime/ConsoleHooks.h"
#include <Windows.h>
#include <ShlObj.h>
#include <algorithm>
#include <array>

namespace fusionmcp::runtime {
namespace {
constexpr std::size_t kMaxLogTailBytes=65536;
std::string Printable(const char* text) {
    std::string copy=text?text:"";
    for (char& c:copy) if (static_cast<unsigned char>(c)<32||static_cast<unsigned char>(c)>126) c='?';
    return copy;
}
// Plain ASCII names only: anything else cannot be passed to the console safely.
std::vector<FileEntry> List(const std::filesystem::path& directory,const auto& accept) {
    std::vector<FileEntry> files;
    std::error_code error;
    if (directory.empty()) return files;
    for (std::filesystem::directory_iterator it(directory,error),end;!error&&it!=end;it.increment(error)) {
        const auto wide=it->path().filename().wstring();
        if (!std::all_of(wide.begin(),wide.end(),[](wchar_t c){return c>=32&&c<=126;})) continue;
        std::string name;
        for (const wchar_t c:wide) name+=static_cast<char>(c);
        if (!accept(name)||!it->is_regular_file(error)) continue;
        const auto bytes=it->file_size(error);
        const auto modified=it->last_write_time(error).time_since_epoch().count();
        if (error) {error.clear();continue;}
        files.push_back({name,it->path().string(),bytes,modified});
        if (files.size()>=4096) break;
    }
    return files;
}
std::string Lower(std::string text) {
    for (char& c:text) if (c>='A'&&c<='Z') c=static_cast<char>(c+('a'-'A'));
    return text;
}
}

// Console::ExecuteCommand and ConsoleLog ids are present for these runtimes in
// redb (official 1.10.155 PDB; Address Library rows for 1.10.163, 1.11.221 and
// 1.11.240). NG 1.10.98x and earlier AE builds are unverified there.
bool ConsoleVerifiedRuntime(const REL::Version& runtime) noexcept {return VerifiedRuntimeSlot(runtime)>=0;}
int VerifiedRuntimeSlot(const REL::Version& runtime) noexcept {
    if (runtime==REL::Version{1,10,163,0}) return 0;
    if (runtime==REL::Version{1,11,221,0}) return 1;
    if (runtime==REL::Version{1,11,240,0}) return 2;
    return -1;
}
namespace {
std::filesystem::path GameDocuments() {
    PWSTR documents=nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents,0,nullptr,&documents))) return {};
    const auto root=std::filesystem::path(documents)/L"My Games"/L"Fallout4";
    CoTaskMemFree(documents);
    return root;
}
}
// Native screenshots stay out of the game folder: Documents\My Games\Fallout4\FusionMCP\Screenshots.
NativeGameServices::NativeGameServices(bool runtimeVerified,int runtimeSlot):runtimeVerified_(runtimeVerified),runtimeSlot_(runtimeSlot),
    world_(runtimeVerified&&runtimeSlot>=0?std::make_unique<NativeWorldServices>(runtimeSlot):nullptr),
    frames_(runtimeVerified?std::make_unique<FrameCapture>(GameDocuments()/L"FusionMCP"/L"Screenshots",[]{return RequestClock::now();}):nullptr) {}
// Documents\My Games\Fallout4: F4SE plugin logs and Papyrus script logs.
std::vector<FileEntry> NativeGameServices::ListLogs() {
    std::vector<FileEntry> logs;
    const auto root=GameDocuments();
    if (root.empty()) return logs;
    for (const auto& [folder,label]:{std::pair{root/L"F4SE","F4SE/"},std::pair{root/L"Logs"/L"Script","Script/"},
            std::pair{root/L"Logs"/L"Script"/L"User","Script/User/"}}) {
        for (auto& file:List(folder,[](const std::string& name){return Lower(name).ends_with(".log");})) {
            file.name=label+file.name;
            logs.push_back(std::move(file));
        }
    }
    return logs;
}

std::vector<ConsoleRow> NativeGameServices::ConsoleCommands() {
    std::vector<ConsoleRow> rows;
    if (!runtimeVerified_) return rows;
    const auto append=[&rows](auto functions,const char* table) {
        for (const auto& function:functions) {
            if (!function.functionName||!*function.functionName) continue;
            rows.push_back({Printable(function.functionName),Printable(function.shortName),Printable(function.helpString),
                table,function.referenceFunction,function.paramCount});
        }
    };
    append(RE::SCRIPT_FUNCTION::GetConsoleFunctions(),"console");
    append(RE::SCRIPT_FUNCTION::GetScriptFunctions(),"script");
    return rows;
}
bool NativeGameServices::ExecuteConsole(const std::string& line) {
    if (!runtimeVerified_) return false;
    RE::Console::ExecuteCommand(line.c_str());
    return true;
}
void NativeGameServices::InstallConsoleHooks() {
    if (!runtimeVerified_||consoleRuntime_) return;
    std::string detail;
    runtime::InstallConsoleHooks(runtimeSlot_,capture_,detail);
    consoleRuntime_=runtimeSlot_>=0;
    REX::INFO("FusionMCP console: {}",detail);
}
ConsoleCapture* NativeGameServices::Capture() noexcept {return ConsolePrintHooked()?&capture_:nullptr;}
ConsoleRunResult NativeGameServices::RunConsoleLine(const std::string& line,bool drain) {
    if (!runtimeVerified_||!consoleRuntime_) return {false,"unavailable","Console runtime is not initialised on this runtime"};
    // Without the echo guard the engine would read a '%' in the line as a printf conversion.
    if (line.find('%')!=std::string::npos&&!ConsoleEchoHooked())
        return {false,"unsafe_line","'%' needs the console echo guard, which is not installed on this runtime build"};
    ConsoleRunResult result;
    result.dispatched=true;
    if (ConsolePrintHooked()) {
        result.capture=capture_.Begin();
        RunConsoleLineNative(line.c_str(),drain);
        const auto snapshot=capture_.Read(result.capture);
        result.output=snapshot.text;result.truncated=snapshot.truncated;result.source="print_hook";
        return result;
    }
    // Fallback: the engine's pending log buffer. It is emptied whenever the console
    // menu shows it and stops growing at 64 KB, so output can be missing.
    const auto before=ReadConsoleLog();
    RunConsoleLineNative(line.c_str(),drain);
    const auto after=ReadConsoleLog();
    result.source="log_buffer";
    if (after.available&&after.size>=before.size) {
        auto delta=static_cast<std::size_t>(after.size-before.size);
        if (delta>after.tail.size()) {delta=after.tail.size();result.truncated=true;}
        result.output=StripConsoleEcho(after.tail.substr(after.tail.size()-delta),line);
    }
    return result;
}
ConsoleLogSnapshot NativeGameServices::ReadConsoleLog() {
    const auto* log=runtimeVerified_?RE::ConsoleLog::GetSingleton():nullptr;
    if (!log) return {};
    ConsoleLogSnapshot snapshot{true,log->buffer.size(),{}};
    if (const auto* data=log->buffer.data();data&&snapshot.size) {
        const auto bytes=std::min<std::size_t>(static_cast<std::size_t>(snapshot.size),kMaxLogTailBytes);
        snapshot.tail.assign(data+snapshot.size-bytes,bytes);
    }
    return snapshot;
}
void NativeGameServices::ResolveSaveDirectory() {
    if (!runtimeVerified_) return;
    auto* manager=RE::BGSSaveLoadManager::GetSingleton();
    if (!manager) return;
    std::array<char,4096> buffer{};
    manager->GetSaveDirectoryPath(buffer.data());
    buffer.back()='\0';
    std::scoped_lock lock(mutex_);
    saveDirectory_=std::filesystem::path(buffer.data());
}
std::string NativeGameServices::SaveDirectory() const {
    std::scoped_lock lock(mutex_);
    return saveDirectory_.string();
}
std::vector<FileEntry> NativeGameServices::ListSaves() {
    std::filesystem::path directory;
    {std::scoped_lock lock(mutex_);directory=saveDirectory_;}
    return List(directory,[](const std::string& name){return Lower(name).ends_with(".fos");});
}
// The engine writes ScreenShot<N>.png next to the executable.
std::vector<FileEntry> NativeGameServices::ListScreenshots() {
    std::array<wchar_t,32768> exe{};
    const auto length=GetModuleFileNameW(nullptr,exe.data(),static_cast<DWORD>(exe.size()));
    if (!length||length>=exe.size()) return {};
    return List(std::filesystem::path(exe.data()).parent_path(),[](const std::string& name) {
        const auto lower=Lower(name);
        return lower.starts_with("screenshot")&&lower.ends_with(".png");
    });
}
bool NativeGameServices::QueueScreenshot() {
    if (!runtimeVerified_) return false;
    const auto* controls=RE::MenuControls::GetSingleton();
    return controls&&controls->QueueScreenshot();
}
ReadinessSample NativeGameServices::SampleReadiness() {
    ReadinessSample sample;
    if (!runtimeVerified_) {sample.reason="Runtime not verified for engine structure reads";return sample;}
    const auto* ui=RE::UI::GetSingleton();
    if (!ui) {sample.reason="UI singleton unavailable";return sample;}
    auto* player=RE::PlayerCharacter::GetSingleton();
    auto* cell=player?player->GetParentCell():nullptr;
    sample.sampled=true;
    sample.loadingMenuOpen=ui->GetMenuOpen<RE::LoadingMenu>();
    sample.mainMenuOpen=ui->GetMenuOpen<RE::MainMenu>();
    sample.playerPresent=player!=nullptr;
    sample.parentCellAttached=cell&&cell->IsAttached();
    sample.player3DLoaded=player&&player->Get3D();
    return sample;
}
}
