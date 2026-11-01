#include "core/IdleWatchdog.h"
#include <algorithm>

namespace fusionmcp {
IdleWatchdog::IdleWatchdog(RequestBroker::NowFn now):now_(std::move(now)),last_(now_()) {}

void IdleWatchdog::Configure(unsigned minutes) {
    std::scoped_lock lock(mutex_);
    minutes_=std::min(minutes,kMaxMinutes);source_=minutes_?"ini":"off";last_=now_();fired_=false;
}
void IdleWatchdog::Arm(unsigned minutes) {
    std::scoped_lock lock(mutex_);
    minutes_=std::min(minutes,kMaxMinutes);source_="session";last_=now_();fired_=false;
}
void IdleWatchdog::Disarm() {
    std::scoped_lock lock(mutex_);
    minutes_=0;source_="off";fired_=false;
}
void IdleWatchdog::Touch() {
    std::scoped_lock lock(mutex_);
    last_=now_();
}
bool IdleWatchdog::Expired() {
    std::scoped_lock lock(mutex_);
    if (!minutes_||fired_||now_()-last_<std::chrono::minutes(minutes_)) return false;
    fired_=true;
    return true;
}
Json IdleWatchdog::StatusLocked() const {
    const auto idle=std::chrono::duration_cast<std::chrono::milliseconds>(now_()-last_).count();
    Json status={{"armed",minutes_>0},{"minutes",minutes_},{"source",source_},{"idleMs",idle},{"fired",fired_}};
    if (minutes_) status["quitInMs"]=std::max<std::int64_t>(0,std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::minutes(minutes_)).count()-idle);
    return status;
}
Json IdleWatchdog::Status() const {std::scoped_lock lock(mutex_);return StatusLocked();}

ProviderRegistry::ControlTool IdleWatchdog::Tool(std::function<void(const std::string&)> log) {
    return {"fusionmcp.watchdog","Idle quit watchdog. When armed, the game quits cleanly (the engine's own quit) if no MCP request "
        "arrives for minutes; every accepted MCP request resets the timer. Stops a stuck test hold when a remote tunnel dies. "
        "action: status (default), arm {minutes 1..1440, for this session; overrides [MCP] IdleQuitMinutes}, disarm.",
        ParseBounded(R"({"type":"object","properties":{"action":{"type":"string","enum":["status","arm","disarm"]},)"
            R"("minutes":{"type":"integer","minimum":1,"maximum":1440}},"additionalProperties":false})"),false,
        [this,log](const Json& args,bool) {
            const auto action=args.value("action",std::string("status"));
            if (action=="arm") {
                if (!args.contains("minutes")) return Error("invalid_arguments","arm needs minutes");
                Arm(args["minutes"].get<unsigned>());
                if (log) log("FusionMCP idle watchdog armed for this session: quit after "+std::to_string(args["minutes"].get<unsigned>())+
                    " min without an MCP request");
            } else if (action=="disarm") {
                Disarm();
                if (log) log("FusionMCP idle watchdog disarmed for this session");
            }
            return Success(Status());
        },"idle_watchdog",{"arm","disarm"}};
}
}
