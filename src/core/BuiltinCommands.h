#pragma once
// Project-owned GPL-3.0 code. Built-in fusionmcp.* Game owner commands. They are
// registered through ProviderRegistry::RegisterBuiltin so they share the
// broker, owner, epoch and lease rules of every provider command.
#include "core/GameServices.h"
#include "core/ProviderRegistry.h"
#include "core/RequestBroker.h"
#include "core/SaveLoadCoordinator.h"
#include <fusionmcp/ProviderAPI.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fusionmcp {
class BuiltinCommands {
public:
    // A console line's own output is complete when the engine returns from it.
    // With waitMs, later prints (batch files) are recorded until kConsoleQuiet
    // passes without one, or the window ends.
    static constexpr auto kConsoleQuiet=std::chrono::milliseconds(500);
    static constexpr unsigned kBatchWindowMs=3000;
    static constexpr std::size_t kMaxConsoleOutputBytes=16384;
    // Leaves room for metadata inside the 256 KiB result envelope.
    static constexpr std::size_t kMaxImageBase64Bytes=240000;
    // The engine must write a new screenshot file within this window.
    static constexpr auto kCaptureWindow=std::chrono::seconds(10);

    BuiltinCommands(std::shared_ptr<GameServices> services,RequestBroker::NowFn now=[]{return RequestClock::now();});
    ~BuiltinCommands();
    BuiltinCommands(const BuiltinCommands&)=delete;
    BuiltinCommands& operator=(const BuiltinCommands&)=delete;
    // Valid for this object's lifetime; the registry copies descriptors.
    const FUSIONMCP_ProviderV1* Provider() const noexcept;
    std::vector<std::string> Capabilities() const;
    // fusionmcp.saves.list and fusionmcp.wait_ready; they never touch an engine owner.
    std::vector<ProviderRegistry::ControlTool> ControlTools();
    // Registers Provider() and ControlTools() with core.
    bool RegisterWith(ProviderRegistry& core);
    SaveLoadCoordinator& Saves() noexcept;
    // Game owner, from each pump under its claimed epoch: readiness samples
    // for a loaded save transaction.
    void SampleOnGame(EpochId epoch);
    bool NeedsGameWake() const;
    // f4fs.* calls for fusionmcp.perf.sample's GPU zones; called only from a B21 worker thread.
    void SetF4fs(std::function<Json(const std::string& tool,const Json& args)> caller);
private:
    struct State;
    std::unique_ptr<State> state_;
};
}
