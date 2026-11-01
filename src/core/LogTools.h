#pragma once
// Project-owned GPL-3.0 code. Log names with '*' wildcards, byte-offset markers
// ("<log>#<offset>", optionally stored under a caller id so a batch can refer to
// them later) and a background regex wait over newly appended lines.
#include "core/GameServices.h"
#include "core/RequestBroker.h"
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <regex>
#include <string>
#include <thread>
#include <vector>

namespace fusionmcp {
// Exact name (case-insensitive), or a pattern with '*' matching the newest log.
std::optional<FileEntry> ResolveLog(const std::vector<FileEntry>& logs,const std::string& nameOrPattern);
bool WildcardMatch(std::string_view pattern,std::string_view text);  // case-insensitive, '*' only

struct LogMarker {std::string log;std::uint64_t offset=0;};
std::string FormatMarker(const LogMarker& marker);
std::optional<LogMarker> ParseMarker(const std::string& text);

class LogMarkers {
public:
    static constexpr std::size_t kMaxStored=64;
    void Store(const std::string& id,const LogMarker& marker);
    // A "<log>#<offset>" marker or the id of a stored one.
    std::optional<LogMarker> Find(const std::string& markerOrId) const;
private:
    mutable std::mutex mutex_;
    std::map<std::string,std::pair<std::uint64_t,LogMarker>> stored_;
    std::uint64_t sequence_=0;
};

// Polls one log file on its own thread until a line matches, the deadline
// passes or Stop. Truncation restarts from 0 (reported as rotated).
class LogWaiter {
public:
    struct Result {
        enum class State {Waiting,Matched,TimedOut,Failed} state=State::Waiting;
        std::string log,line,error;
        std::uint64_t startOffset=0,lineOffset=0,nextOffset=0,scanned=0;
        bool rotated=false;
    };
    using Lister=std::function<std::vector<FileEntry>()>;
    static constexpr std::size_t kMaxLineBytes=4096;
    LogWaiter(Lister list,std::string log,std::regex pattern,std::optional<std::uint64_t> fromOffset,
        std::chrono::milliseconds timeout,std::chrono::milliseconds interval=std::chrono::milliseconds(50));
    ~LogWaiter();
    Result Poll() const;
    void Stop();
private:
    void Run(std::stop_token stop);
    Lister list_;
    std::string log_;
    std::regex pattern_;
    std::optional<std::uint64_t> from_;
    std::chrono::milliseconds timeout_,interval_;
    mutable std::mutex mutex_;
    Result result_;
    std::jthread worker_;
};
}
