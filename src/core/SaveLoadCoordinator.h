#pragma once
// Project-owned GPL-3.0 code. Engine-free named-save load transaction. Unlike an
// ordinary request it survives the epoch changes its own load causes; it binds
// only the one resulting epoch that F4SE load messages correlate to its target.
#include "core/GameServices.h"
#include "core/Json.h"
#include "core/RequestBroker.h"
#include <mutex>
#include <optional>

namespace fusionmcp {
class SaveLoadCoordinator {
public:
    static constexpr auto kTimeout=std::chrono::seconds(180);
    static constexpr unsigned kStableSamples=3;
    static constexpr auto kStableSpan=std::chrono::milliseconds(750);

    explicit SaveLoadCoordinator(RequestBroker::NowFn now=[]{return RequestClock::now();});
    // Exact, case-insensitive match against *.fos stems; refuses missing and
    // ambiguous names and a second active transaction. Returns the save entry.
    Json Validate(const std::string& save,const std::vector<FileEntry>& saves) const;
    // Records a transaction for a validated save just before engine dispatch.
    std::uint64_t Begin(const std::string& save,EpochId epoch);
    void Dispatched(std::uint64_t id,bool accepted);
    // F4SE kPreLoadGame (save file path) and kPostLoadGame (success flag).
    void OnPreLoad(std::string_view path);
    void OnPostLoad(bool success,EpochId resultEpoch);
    void OnNewGame();
    // Game owner sample under the pump's claimed epoch.
    void Sample(const ReadinessSample& sample,EpochId epoch);
    bool NeedsSamples() const;
    // Most recent transaction when id is 0. Null when none is retained.
    Json Snapshot(std::uint64_t id) const;
    Json Cancel(std::uint64_t id);
private:
    struct Transaction {
        std::uint64_t id=0;
        std::string save,phase="validated",reason;
        EpochId startEpoch=0,resultEpoch=0;
        RequestTime begun,firstReady;
        unsigned readySamples=0,samples=0;
        bool terminal=false;
    };
    Transaction* Find(std::uint64_t id);
    const Transaction* Find(std::uint64_t id) const;
    Transaction* Active();
    void Expire(Transaction& t) const;
    void Finish(Transaction& t,std::string phase,std::string reason);
    Json Describe(const Transaction& t) const;
    mutable std::mutex mutex_;
    RequestBroker::NowFn now_;
    std::vector<Transaction> history_;
    std::uint64_t next_=1;
};
}
