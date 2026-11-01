#pragma once
// Project-owned GPL-3.0 code. Engine-free seam for built-in Game owner commands.
// Every method runs on the Game owner. Implementations copy engine state into
// these DTOs and never hand out engine pointers.
#include <cstdint>
#include <string>
#include <vector>

namespace fusionmcp {
class WorldServices;
class ConsoleCapture;
class FrameCapture;
struct ConsoleRow {
    std::string name, alias, help;
    std::string table;           // "console" or "script"
    bool referenceFunction=false;
    std::uint16_t paramCount=0;
};
struct ConsoleLogSnapshot {
    bool available=false;
    std::uint64_t size=0;        // Total buffer bytes at sample time.
    std::string tail;            // Last bytes of the buffer, bounded by the adapter.
};
struct ConsoleRunResult {
    bool dispatched=false;
    std::string errorCode,errorMessage;  // set when not dispatched
    // Nonzero: output keeps arriving in Capture() under this generation until
    // the caller ends it (deferred printers such as batch files). Zero: output
    // is complete in the fields below.
    std::uint64_t capture=0;
    std::string output;                  // printed during the call, engine echo removed
    bool truncated=false;
    std::string source;                  // "print_hook" or "log_buffer"
};
struct ReadinessSample {
    bool sampled=false;
    bool loadingMenuOpen=false,mainMenuOpen=false,playerPresent=false,parentCellAttached=false,player3DLoaded=false;
    std::string reason;
};
struct FileEntry {
    std::string name;            // File name without directory.
    std::string path;            // Absolute path, host-owned directory only.
    std::uint64_t bytes=0;
    std::int64_t modified=0;     // Implementation-defined monotonic file time.
};
class GameServices {
public:
    virtual ~GameServices()=default;
    virtual bool ConsoleSupported() const noexcept=0;
    // Filesystem only, any thread: *.fos in the engine's save directory and
    // engine screenshot PNGs. Empty when the directory is not yet known.
    virtual std::vector<FileEntry> ListSaves()=0;
    virtual std::vector<FileEntry> ListScreenshots()=0;
    // Game owner. Sets the engine's own screenshot request; false if one is
    // already queued or the handler is unavailable. Not a capture completion.
    virtual bool QueueScreenshot()=0;
    virtual std::vector<ConsoleRow> ConsoleCommands()=0;
    // Hands one line to the engine's console queue. true is a dispatch
    // acknowledgement only: the engine executes queued commands later.
    virtual bool ExecuteConsole(const std::string& line)=0;
    virtual ConsoleLogSnapshot ReadConsoleLog()=0;
    // Game owner. Runs one line exactly as typed into the console input: the
    // engine's ExecuteCommand (echo, macros, ';' statements, ForEachRef, queue)
    // and then, with drain, its own queue drain, which compiles and runs the line
    // with the console's script compiler and selected reference before returning.
    // Without drain the engine runs the queued line on its next frame.
    virtual ConsoleRunResult RunConsoleLine(const std::string&,bool drain=true) {
        (void)drain;return {false,"unsupported","Full console lines are unavailable on this runtime"};
    }
    // Print capture fed by the adapter's console print hook, when installed.
    virtual ConsoleCapture* Capture() noexcept {return nullptr;}
    virtual ReadinessSample SampleReadiness()=0;
    // World inspection/control, when the adapter supports it on this runtime.
    virtual WorldServices* World() noexcept {return nullptr;}
    // Native present-time frame capture (thread-safe, any thread), when the
    // runtime provides one. Its own Installed() says whether frames arrive.
    virtual FrameCapture* Frames() noexcept {return nullptr;}
    // Filesystem only, any thread: readable log files (Papyrus and F4SE plugin logs).
    // name is a stable relative label such as "F4SE/FusionMCP.log".
    virtual std::vector<FileEntry> ListLogs() {return {};}
};
}
