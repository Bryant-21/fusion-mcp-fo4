#pragma once
// Project-owned GPL-3.0 code. fusionmcp.* testing commands: exact camera poses and
// native camera paths, per-region frame statistics, frame time sampling, log
// waits, placed test references and a clean quit. Hosted by BuiltinCommands in
// the single "fusionmcp" provider, so they share its broker, owner, epoch and lease rules.
#include "core/CameraMath.h"
#include "core/FrameCapture.h"
#include "core/GameServices.h"
#include "core/Json.h"
#include "core/LogTools.h"
#include "core/RequestBroker.h"
#include "core/WorldServices.h"
#include <fusionmcp/ProviderAPI.h>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fusionmcp {
class TestCommands {
public:
    // Calls one f4fs.* tool through F4FS's RemoteControl API. Only ever invoked
    // from a FusionMCP worker thread, never from an engine owner.
    using F4fsCaller=std::function<Json(const std::string& tool,const Json& args)>;
    static constexpr auto kStallLimit=std::chrono::milliseconds(3000);
    static constexpr auto kPlaceWindow=std::chrono::milliseconds(5000);
    static constexpr float kPlaceSearchRadius=1500;
    // Game pumps before a moved or switched reference is read back: the engine applies
    // SetPos/Enable/Disable from the console on a later frame than the line runs.
    static constexpr unsigned kRefSettlePolls=3;
    struct Spec {std::string name,description;const char* schema;uint32_t flags;};
    static std::vector<Spec> Specs();

    TestCommands(WorldServices& world,GameServices& game,LogMarkers& markers,RequestBroker::NowFn now);
    ~TestCommands();
    void SetF4fs(F4fsCaller caller) {f4fs_=std::move(caller);}
    // Game owner. nullopt: not a testing command / operation.
    std::optional<FUSIONMCP_Status> Invoke(const std::string& command,const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink);
    std::optional<FUSIONMCP_Status> Poll(const std::string& operation,FUSIONMCP_WriteFn writer,void* sink);
    bool Cancel(const std::string& operation);
    // Game owner, every pump: keeps the idle vanity camera locked off.
    void SampleOnGame();
    bool NeedsGameWake() const noexcept {return vanityLocked_;}
private:
    struct PathState;
    struct GpuJob;
    enum class Kind {Path,Stats,Perf,LogWait,Place,RefSettle};
    struct Operation {
        Kind kind=Kind::Path;
        RequestTime started;
        std::shared_ptr<PathState> path;
        bool restoreHud=false;
        unsigned settlePolls=0;
        std::uint64_t frameId=0;
        FrameCapture::Options capture;
        std::uint64_t durationMs=0;
        bool gpu=false;
        std::shared_ptr<GpuJob> job;
        Json result;
        std::unique_ptr<LogWaiter> waiter;
        std::uint32_t base=0,ref=0;
        CameraPose target;
        std::set<std::uint32_t> before;
    };
    Json CameraJson(const CameraSnapshot& s) const;
    std::optional<FUSIONMCP_Status> CameraSet(const Json& args,FUSIONMCP_WriteFn writer,void* sink);
    std::optional<FUSIONMCP_Status> StartCameraPath(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink);
    std::optional<FUSIONMCP_Status> Place(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink);
    std::optional<std::uint32_t> FindPlaced(const Operation& o);
    // Moves the placed reference; the operation then reports it after kRefSettlePolls.
    std::optional<Json> MovePlaced(std::uint32_t ref,Operation& o);
    std::string RunConsole(const std::string& line);
    void Finish(Operation& o);
    WorldServices& world_;
    GameServices& game_;
    LogMarkers& markers_;
    RequestBroker::NowFn now_;
    F4fsCaller f4fs_;
    std::map<std::string,Operation> operations_;
    bool vanityLocked_=false;
};
}
