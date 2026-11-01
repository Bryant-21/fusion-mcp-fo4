#pragma once
// Project-owned GPL-3.0 code. Quits the game cleanly when no accepted MCP request
// arrived for a configured time, so a test hold cannot outlive a dead remote
// tunnel. Default off: the INI value or a per-session arm enables it.
#include "core/ProviderRegistry.h"
#include "core/RequestBroker.h"
#include <functional>
#include <mutex>

namespace fusionmcp {
class IdleWatchdog {
public:
    static constexpr unsigned kMaxMinutes=24*60;
    explicit IdleWatchdog(RequestBroker::NowFn now=[]{return RequestClock::now();});
    // From the INI at startup; 0 leaves it off. Counts from this call.
    void Configure(unsigned minutes);
    void Arm(unsigned minutes);
    void Disarm();
    void Touch();
    // True exactly once when armed and idle for the limit; the caller then quits.
    bool Expired();
    Json Status() const;
    // fusionmcp.watchdog: status, arm {minutes}, disarm.
    ProviderRegistry::ControlTool Tool(std::function<void(const std::string&)> log);
private:
    Json StatusLocked() const;
    RequestBroker::NowFn now_;
    mutable std::mutex mutex_;
    unsigned minutes_=0;
    std::string source_="off";
    RequestTime last_;
    bool fired_=false;
};
}
