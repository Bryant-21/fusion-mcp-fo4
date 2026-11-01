#pragma once
// Project-owned GPL-3.0 code. fusionmcp.* world inspection and control commands over
// WorldServices. Hosted by BuiltinCommands inside the single "fusionmcp" provider, so
// they share its broker, owner, epoch and lease rules.
#include "core/GameServices.h"
#include "core/Json.h"
#include "core/RequestBroker.h"
#include "core/WorldServices.h"
#include <fusionmcp/ProviderAPI.h>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fusionmcp {
class WorldCommands {
public:
    static constexpr std::size_t kMaxAnimEvents=500;
    static constexpr auto kDefaultWait=std::chrono::milliseconds(30000);
    static constexpr unsigned kStableSamples=2;
    static constexpr std::uint32_t kPlayerRef=0x14;
    static constexpr auto kSpawnWindow=std::chrono::milliseconds(5000);
    static constexpr float kSpawnSearchRadius=1500;
    static constexpr std::size_t kSpawnScanLimit=2000;

    struct Spec {std::string name,description;const char* schema;uint32_t flags;};
    static std::vector<Spec> Specs();

    WorldCommands(WorldServices& world,GameServices& game,RequestBroker::NowFn now);
    ~WorldCommands();
    // Game owner. nullopt: not a world command / operation.
    std::optional<FUSIONMCP_Status> Invoke(const std::string& command,const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink);
    std::optional<FUSIONMCP_Status> Poll(const std::string& operation,FUSIONMCP_WriteFn writer,void* sink);
    bool Cancel(const std::string& operation);
private:
    enum class Kind {Hold,Wait,Watch,Spawn};
    struct Operation {
        Kind kind=Kind::Hold;
        RequestTime started,deadline;
        std::vector<Control> controls;
        std::string condition,menu;
        std::uint32_t target=0;
        float radius=0;
        unsigned stable=0,polls=0;
        std::uint64_t watchId=0;
        std::string contains;
        std::vector<AnimEvent> events;
        std::size_t dropped=0;
        std::uint32_t base=0,count=0;
        std::set<std::uint32_t> before;
    };
    std::optional<std::uint32_t> Target(const Json& args,const char* key,const char* fallback,Json& error);
    bool ConditionMet(Operation& o,std::string& observed);
    void Finish(Operation& o);
    WorldServices& world_;
    GameServices& game_;
    RequestBroker::NowFn now_;
    std::map<std::string,Operation> operations_;
    std::uint64_t nextWatch_=1;
};
}
