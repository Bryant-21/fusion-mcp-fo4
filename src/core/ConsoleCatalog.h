#pragma once
// Project-owned GPL-3.0 code. Engine-free console command inventory and typed
// line assembly. Native rows are copied once on the Game owner.
#include "core/GameServices.h"
#include "core/Json.h"
#include <optional>
#include <string_view>

namespace fusionmcp {
// Native Console::ExecuteCommand expands into a 1024-byte buffer.
inline constexpr std::size_t kMaxConsoleLineBytes=1000;
inline constexpr std::size_t kMaxConsoleArguments=16;
inline constexpr std::size_t kMaxConsoleStringBytes=256;

enum class ConsoleResolution { Unique, Missing, Ambiguous };
struct ConsoleMatch {
    ConsoleResolution status=ConsoleResolution::Missing;
    const ConsoleRow* row=nullptr;
    std::size_t candidates=0;
};
class ConsoleCatalog {
public:
    explicit ConsoleCatalog(std::vector<ConsoleRow> rows);
    // Case-insensitive name or alias. Duplicate rows (e.g. two canonical
    // LoadGame entries) are Ambiguous; there is no first-match fallback.
    ConsoleMatch Resolve(std::string_view token) const;
    std::vector<const ConsoleRow*> Candidates(std::string_view token) const;
    Json List(std::string_view filter,std::size_t offset,std::size_t limit) const;
    std::size_t Size() const noexcept {return rows_.size();}
private:
    std::vector<ConsoleRow> rows_;
};
// Rows whose effect outlives the line: load, save, quit, batch files. They run
// like any other line; the flag tells clients what to expect.
bool IsLifecycleCommand(const ConsoleRow& row);
bool IsLoadCommand(const ConsoleRow& row);
// bat / RunConsoleBatch: the console menu runs the file's lines on later frames.
bool IsBatchCommand(const ConsoleRow& row);
struct ConsoleLine {
    std::string text;
    std::string errorCode,errorMessage;
};
// arguments: schema-validated {command,target?,arguments?}. row is the resolved
// row (MergeRows for same-named duplicates).
ConsoleLine AssembleConsoleLine(const ConsoleRow& row,const Json& arguments);
// Duplicate rows that share one canonical name (two LoadGame rows) run as that
// name; the merged row accepts the larger parameter count.
std::optional<ConsoleRow> MergeRows(const std::vector<const ConsoleRow*>& rows);

// A full console line as typed into the console input. The engine copies it into
// 1024-byte buffers (with room for macro expansion), so lines are capped lower.
// Control characters cannot be typed into the console and are refused.
ConsoleLine ValidateConsoleLine(std::string_view line);
// The leading statement of a line: "[<ref>.]<command> <argument...>". The engine
// splits statements on ';' (ProcessCommand), so single is false when one appears.
struct ConsoleStatement {
    std::string target,command,argument;  // argument: rest of the statement, outer quotes removed
    bool single=false;
};
ConsoleStatement ParseConsoleStatement(std::string_view line);
// Text the engine prints for a line itself: its raw echo and ProcessCommand's
// "Command: <line>" (only while the console menu is closed). Removed from the
// front of observed output.
std::string StripConsoleEcho(std::string output,std::string_view line);
}
