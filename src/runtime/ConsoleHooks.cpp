// Original project-owned console print capture over CommonLibF4. Each patched call
// site is checked to hold a rel32 call to the expected engine function first.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include "runtime/ConsoleHooks.h"
#include "GameIDs.h"
#include <algorithm>
#include <vector>
#include <cstring>
#include <string_view>

namespace fusionmcp::runtime {
namespace {
using AddStringFn=void(*)(void*,const char*);
using PrintFn=void(*)(void*,const char*,...);
using DrainFn=void(*)();
struct Hooks {
    ConsoleCapture* capture=nullptr;
    AddStringFn addString=nullptr;
    PrintFn print=nullptr;
    DrainFn drain=nullptr;
    bool printHooked=false,echoHooked=false;
};
Hooks& State() {static Hooks hooks;return hooks;}
// Set only on the Game owner while it dispatches its own line.
thread_local bool dispatching=false;
thread_local int echoing=0;

void HookedAddString(void* log,const char* text) {
    auto& h=State();
    if (text&&*text&&h.capture&&!echoing) {
        const std::string_view view(text);
        // ProcessCommand prints the line once more as "Command: <line>" when the console menu is closed.
        if (!(dispatching&&view.starts_with("Command: "))) h.capture->Append(view);
    }
    h.addString(log,text);
}
// The engine passes the typed line as the format string; print it as data.
void HookedEcho(void* log,const char* line) {
    auto& h=State();
    if (dispatching) ++echoing;
    h.print(log,"%s",line?line:"");
    if (dispatching) --echoing;
}
bool CallTarget(std::uintptr_t site,std::uintptr_t expected) {
    const auto* bytes=reinterpret_cast<const std::uint8_t*>(site);
    if (bytes[0]!=0xE8) return false;
    std::int32_t rel=0;
    std::memcpy(&rel,bytes+1,sizeof(rel));
    return site+5+static_cast<std::intptr_t>(rel)==expected;
}
}

bool InstallConsoleHooks(int slot,ConsoleCapture& capture,std::string& detail) {
    auto& h=State();
    if (h.drain) return h.printHooked;
    if (slot<0) {detail="runtime not verified";return false;}
    namespace console=ids::Console;
    const auto addString=console::AddString.address();
    h.capture=&capture;
    h.addString=reinterpret_cast<AddStringFn>(addString);
    h.print=reinterpret_cast<PrintFn>(console::PrintLine.address());
    h.drain=reinterpret_cast<DrainFn>(console::ExecuteQueuedCommand.address());
    auto& trampoline=REL::GetTrampoline();
    std::vector<std::uintptr_t> calls{console::PrintAddStringCall.address()};
    if (console::SecondAddStringCall.offset.value()) calls.push_back(console::SecondAddStringCall.address());
    if (std::all_of(calls.begin(),calls.end(),[&](std::uintptr_t call){return CallTarget(call,addString);})) {
        for (const auto call:calls) trampoline.write_call<5>(call,&HookedAddString);
        h.printHooked=true;
    }
    if (const auto echo=console::ExecuteCommandEchoCall.address();CallTarget(echo,console::PrintLine.address())) {
        trampoline.write_call<5>(echo,&HookedEcho);
        h.echoHooked=true;
    }
    detail=std::string("print capture ")+(h.printHooked?"installed":"unavailable (ConsoleLog::Print call site differs)")+
        ", echo guard "+(h.echoHooked?"installed":"unavailable (Console::ExecuteCommand call site differs)");
    return h.printHooked;
}
bool ConsolePrintHooked() noexcept {return State().printHooked;}
bool ConsoleEchoHooked() noexcept {return State().echoHooked;}
void RunConsoleLineNative(const char* line,bool drain) {
    dispatching=true;
    RE::Console::ExecuteCommand(line);
    dispatching=false;
    if (drain&&State().drain) State().drain();
}
}
