#pragma once
// Project-owned GPL-3.0 code. Console print capture and echo safety; DLL only.
#include "core/ConsoleCapture.h"
#include <string>

namespace fusionmcp::runtime {
// Patches, after checking each call site holds the expected engine call:
//  - ConsoleLog::Print -> ConsoleLog::AddString (every console print) to feed capture;
//  - Console::ExecuteCommand's echo of the raw line, which the engine passes as a
//    printf format, to print it through "%s" instead (a '%' in a line is then safe).
// runtimeSlot: 0 = 1.10.163, 1 = 1.11.221, 2 = 1.11.240. Call once at plugin load.
bool InstallConsoleHooks(int runtimeSlot,ConsoleCapture& capture,std::string& detail);
bool ConsolePrintHooked() noexcept;
bool ConsoleEchoHooked() noexcept;
// Game owner. Console::ExecuteCommand with capture of its own echo and
// "Command: <line>" print suppressed, then (drain) Console::ExecuteQueuedCommand.
void RunConsoleLineNative(const char* line,bool drain);
}
