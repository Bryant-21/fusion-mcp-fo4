#pragma once
// Project-owned GPL-3.0 code. Ordered batches over the central registry. Each
// step is admitted through ProviderRegistry::Invoke only when the previous one
// reached an actual terminal state; delays are monotonic time, never owner sleeps.
#include "core/ProviderRegistry.h"
#include <mutex>

namespace fusionmcp {
class BatchCoordinator {
public:
    static constexpr std::size_t kMaxSteps=32;
    static constexpr std::size_t kMaxRetained=16;
    static constexpr std::size_t kMaxStepResultBytes=6144;
    static constexpr auto kDefaultStepTimeout=std::chrono::seconds(30);
    static constexpr auto kOverallTimeout=std::chrono::minutes(10);

    explicit BatchCoordinator(ProviderRegistry& core,RequestBroker::NowFn now=[]{return RequestClock::now();});
    // fusionmcp.batch.execute, fusionmcp.batch.get and fusionmcp.batch.cancel.
    std::vector<ProviderRegistry::ControlTool> ControlTools();
    bool RegisterWith(ProviderRegistry& core);
    // Advances running batches. Call from a non-owner thread (transport tick).
    void Tick();
    bool Running() const;
private:
    struct Step {
        std::string tool,state="not_run";
        Json arguments,result;
        std::uint64_t requestId=0,delayMs=0,timeoutMs=0;
        EpochId epochStart=0,epochEnd=0;
        RequestTime started,ended,resumeAt;
        bool polling=false,delaying=false;
    };
    struct Batch {
        std::uint64_t id=0;
        std::string state="running",reason;
        std::vector<Step> steps;
        std::size_t current=0;
        bool allowMutations=false,continueOnFailure=false,transition=false;
        EpochId baseline=0;
        RequestTime begun;
    };
    Json Execute(const Json& args,bool allowMutations);
    Json Get(std::uint64_t id);
    Json Cancel(std::uint64_t id);
    void Advance(Batch& b);
    void Complete(Batch& b,Step& s,std::string state,Json result);
    Json Describe(const Batch& b) const;
    ProviderRegistry& core_;
    RequestBroker::NowFn now_;
    mutable std::mutex mutex_;
    std::vector<Batch> batches_;
    std::uint64_t next_=1;
};
}
