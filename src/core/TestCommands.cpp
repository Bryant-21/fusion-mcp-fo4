#include "core/TestCommands.h"
#include "core/ConsoleCapture.h"
#include "core/Measurements.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <mutex>
#include <thread>

namespace fusionmcp {
namespace {
#define B21_VEC R"({"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"],"additionalProperties":false})"
#define B21_POSE_FIELDS R"("position":)" B21_VEC R"(,"yaw":{"type":"number","minimum":-3600,"maximum":3600},"pitch":{"type":"number","minimum":-89.9,"maximum":89.9})"
constexpr const char* kEmpty=R"({"type":"object","properties":{},"additionalProperties":false})";
// Accepts a fusionmcp.camera.get result unchanged, so a saved pose replays exactly; its
// read-only fields (cameraPosition, player, ...) are ignored.
constexpr const char* kCameraSetSchema=R"({"type":"object","properties":{
"mode":{"enum":["first","third","free"],"description":"Default: the current mode"},)" B21_POSE_FIELDS R"(,
"fov":{"type":"object","properties":{"world":{"type":"number","minimum":10,"maximum":170},"firstPerson":{"type":"number","minimum":10,"maximum":170}},"additionalProperties":false},
"lockVanity":{"type":"boolean","description":"Default true: keep the idle third-person vanity camera from starting"},
"freeCamera":{"type":"boolean"},"cameraPosition":{"type":"object","additionalProperties":true},"player":{"type":"object","additionalProperties":true},
"vanityLocked":{"type":"boolean"},"autoVanityAllowed":{"type":"boolean"},"note":{"type":"string"}},"additionalProperties":false})";
constexpr const char* kCameraPathSchema=R"({"type":"object","properties":{
"keyframes":{"type":"array","minItems":1,"maxItems":64,"items":{"type":"object","properties":{"atMs":{"type":"number","minimum":0,"maximum":120000},)" B21_POSE_FIELDS R"(},"required":["atMs"],"additionalProperties":false}},
"interpolation":{"enum":["linear","slerp"]},
"walk":{"type":"object","properties":{"start":{"type":"object","properties":{)" B21_POSE_FIELDS R"(},"additionalProperties":false},
"speed":{"type":"number","minimum":-10000,"maximum":10000},"yawRate":{"type":"number","minimum":-720,"maximum":720},
"durationMs":{"type":"number","minimum":1,"maximum":120000}},"required":["durationMs"],"additionalProperties":false},
"shots":{"type":"array","maxItems":32,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":40},"atMs":{"type":"number","minimum":0,"maximum":120000}},"required":["name","atMs"],"additionalProperties":false}},
"frameStepMs":{"type":"number","minimum":1,"maximum":100},
"hideHud":{"type":"boolean"},
"timeoutMs":{"type":"integer","minimum":1,"maximum":600000,"description":"Request deadline beyond the default 30 s; needed for paths longer than 25 s"}},"additionalProperties":false})";
constexpr const char* kStatsSchema=R"({"type":"object","properties":{
"regions":{"type":"array","minItems":1,"maxItems":16,"items":{"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":40},
"x":{"type":"number","minimum":0,"maximum":1},"y":{"type":"number","minimum":0,"maximum":1},"w":{"type":"number","minimum":0,"maximum":1},"h":{"type":"number","minimum":0,"maximum":1}},
"required":["name","x","y","w","h"],"additionalProperties":false}},
"reference":{"oneOf":[{"type":"string","minLength":1,"maxLength":40},{"type":"number","minimum":0}],"description":"Region name, or a linear luminance, that is EV 0"},
"whiteThreshold":{"type":"number","minimum":0,"maximum":1},"blackThreshold":{"type":"number","minimum":0,"maximum":1},
"transfer":{"enum":["srgb","pq","linear"]},"hideHud":{"type":"boolean"},"label":{"type":"string","minLength":1,"maxLength":40},
"source":{"enum":["backbuffer","f4fs_hdr"]}},"additionalProperties":false})";
constexpr const char* kPerfSchema=R"({"type":"object","properties":{
"durationMs":{"type":"integer","minimum":100,"maximum":60000},"gpuZones":{"type":"boolean"},
"timeoutMs":{"type":"integer","minimum":1,"maximum":600000}},"additionalProperties":false})";
constexpr const char* kLogWaitSchema=R"({"type":"object","properties":{
"name":{"type":"string","minLength":1,"maxLength":128,"description":"Log name from fusionmcp.logs.list; '*' picks the newest match, e.g. F4SE/F4FS*.log"},
"pattern":{"type":"string","minLength":1,"maxLength":256,"description":"ECMAScript regular expression searched in each new line"},
"since":{"type":"string","minLength":1,"maxLength":200,"description":"Marker or stored marker id from fusionmcp.logs.mark; default: the end of the log when the Game owner starts the wait, a frame or more after the call. To be sure to catch a line that a following call causes, fusionmcp.logs.mark first and pass since"},
"ignoreCase":{"type":"boolean"},"timeoutMs":{"type":"integer","minimum":1,"maximum":600000}},"required":["pattern"],"additionalProperties":false})";
constexpr const char* kPlaceSchema=R"({"type":"object","properties":{
"base":{"type":"string","minLength":1,"maxLength":128},
"relativeTo":{"enum":["player","camera","world"],"description":"Default player"},
"position":)" B21_VEC R"(,
"offset":{"type":"object","properties":{"forward":{"type":"number"},"right":{"type":"number"},"up":{"type":"number"}},"additionalProperties":false},
"yaw":{"type":"number","minimum":-3600,"maximum":3600}},"required":["base"],"additionalProperties":false})";
constexpr const char* kRefSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128},
"action":{"enum":["enable","disable","delete"]}},"required":["target","action"],"additionalProperties":false})";
#undef B21_POSE_FIELDS
#undef B21_VEC

FUSIONMCP_Status Write(FUSIONMCP_WriteFn writer,void* sink,const Json& envelope) {
    const auto bytes=envelope.dump();
    if (bytes.size()>FUSIONMCP_MAX_RESULT_BYTES) return FUSIONMCP_INTERNAL_FAILURE;
    return writer(sink,bytes.data(),static_cast<uint32_t>(bytes.size()))?FUSIONMCP_OK:FUSIONMCP_WRITER_FAILED;
}
FUSIONMCP_Status Pending(FUSIONMCP_WriteFn writer,void* sink,const std::string& id) {
    return writer(sink,id.data(),static_cast<uint32_t>(id.size()))?FUSIONMCP_PENDING:FUSIONMCP_WRITER_FAILED;
}
constexpr std::uint32_t kPlayerRef=0x14;
double Ms(RequestTime from,RequestTime to) {return std::chrono::duration<double,std::milli>(to-from).count();}
std::string Hex(std::uint32_t value) {char b[16];std::snprintf(b,sizeof(b),"%08X",value);return b;}
// Shortest text that reads back as the same float.
std::string Num(double value) {char b[32];std::snprintf(b,sizeof(b),"%.9g",value);return b;}
std::string Printable(std::string text) {
    for (char& c:text) {const auto u=static_cast<unsigned char>(c);if (u<32||u>126) c='?';}
    return text;
}
Json PoseJson(const CameraPose& p) {return {{"position",ToJson(p.position)},{"yaw",p.yaw},{"pitch",p.pitch}};}
Json RefJson(const RefInfo& r) {
    return {{"refId",Hex(r.formId)},{"base",Hex(r.base.formId)},{"baseEditorId",Printable(r.base.editorId)},{"type",Printable(r.base.formType)},
        {"position",ToJson(r.position)},{"angle",ToJson(r.angle)},{"enabled",r.enabled},{"deleted",r.deleted},{"has3D",r.has3D},
        {"cell",Printable(r.cell)}};
}
Json ArtifactJson(const FrameCapture::Status& s) {
    const auto& a=s.artifact;
    return {{"name",s.name},{"path",Printable(s.path)},{"mimeType","image/png"},{"bytes",a.bytes},{"width",a.width},{"height",a.height},
        {"sha256",a.sha256}};
}
std::optional<std::uint32_t> ParseHexId(const std::string& text) {
    if (text.size()!=8) return std::nullopt;
    try {std::size_t used=0;const auto v=std::stoul(text,&used,16);if (used==8) return static_cast<std::uint32_t>(v);} catch (...) {}
    return std::nullopt;
}
}

struct TestCommands::PathState {
    std::mutex mutex;
    CameraPathRun run;
    std::optional<RequestTime> first;
    RequestTime last;
    explicit PathState(CameraPath path):run(std::move(path)) {}
};
struct TestCommands::GpuJob {
    std::atomic_bool done{false};
    Json result;
};

std::vector<TestCommands::Spec> TestCommands::Specs() {
    constexpr uint32_t mutating=FUSIONMCP_COMMAND_MUTATING,async=FUSIONMCP_COMMAND_ASYNC;
    return {
        {"camera.get","Exact camera pose: mode (first, third, free), position/yaw/pitch that fusionmcp.camera.set takes (the free camera's own "
            "pose in free mode, otherwise the player's position and angles), FOV (world, first person), the rendered camera position, "
            "and whether the idle vanity camera is locked off. Pass the result to fusionmcp.camera.set to restore it exactly.",kEmpty,0},
        {"camera.set","Set the camera exactly and repeatably. mode free writes the free camera's position and yaw/pitch directly; first "
            "or third moves the player (SetPos) and sets its angles. fov {world, firstPerson} through the engine's SetCameraFOV. "
            "Missing fields keep their current value. Also locks the idle vanity camera off at runtime (lockVanity=false releases it). "
            "Accepts a fusionmcp.camera.get result unchanged.",kCameraSetSchema,mutating},
        {"camera.path","Async native camera motion in the free camera, applied by the engine's own free camera update every frame: "
            "no input events, works in a background window. keyframes [{atMs, position, yaw, pitch}] with linear or slerp "
            "interpolation, or walk {start?, speed units/s, yawRate deg/s, durationMs}. shots [{name, atMs}] take native screenshots "
            "of the frames at those path times (mid-motion; for flicker checks). frameStepMs advances the path a fixed time per "
            "frame instead of wall time. Ends in the free camera at the final pose.",kCameraPathSchema,mutating|async},
        {"capture.stats","Async: capture the next presented frame (native backbuffer readback, also saved as a PNG whose name is "
            "returned) and measure named regions (normalized x,y,w,h; default the full frame) of that same frame: mean linear "
            "luminance (Rec.709), mean linear RGB, R/G ratio, EV relative to a reference region or luminance, % near-white and % "
            "near-black (max channel of the display value against whiteThreshold 0.98 / blackThreshold 0.02). transfer: srgb "
            "(default), pq for HDR10 swap chains, linear. source f4fs_hdr (F4FS pre-tonemap HDR target) reports why it is unavailable.",
            kStatsSchema,mutating|async},
        {"perf.sample","Async: frame times over durationMs (default 5000) from the game's own presents: count, mean, p50, p99, p99.9, "
            "max and fps. gpuZones (default true) adds F4FS's GPU zone timings (f4fs.inspect kind=perf, the ring behind "
            "[Overlay] iMode=1) and says why when they are unavailable.",kPerfSchema,async},
        {"logs.wait","Async: wait for a line matching pattern in a log (name may use '*', e.g. F4SE/F4FS*.log for the newest) "
            "that is appended after since (a fusionmcp.logs.mark marker or its id) or after the Game owner starts the wait (a frame or more after the call: mark first to catch a line a following call causes). Completes with the line and "
            "the marker after it, or fails at timeoutMs (default 30000).",kLogWaitSchema,async},
        {"game.quit","Quit the game cleanly through the engine's own quit (the main loop exits after this frame, as QuitGame does). "
            "The request may not answer before the process exits.",kEmpty,mutating},
        {"world.place","Async: place one reference of a base form at a pose and return its reference id. relativeTo player "
            "(default) or camera with offset {forward, right, up} (default 150 forward; camera offsets follow its pitch), or world "
            "with position. yaw defaults to the anchor's yaw. Placed by the engine's PlaceAtMe, then moved with SetPos/SetAngle.",
            kPlaceSchema,mutating|async},
        {"world.ref","Async: enable, disable or delete a reference (e.g. switch a placed test light on and off). delete only "
            "removes references created in this game session (FF...... ids): disable, then MarkForDelete. The result reads the "
            "reference back a few frames later, after the engine applied the change.",kRefSchema,mutating|async},
    };
}

TestCommands::TestCommands(WorldServices& world,GameServices& game,LogMarkers& markers,RequestBroker::NowFn now)
    :world_(world),game_(game),markers_(markers),now_(std::move(now)) {}
TestCommands::~TestCommands() {
    std::string ignored;
    for (auto& [id,o]:operations_) if (o.kind==Kind::Path) world_.SetCameraDriver({},ignored);
}

std::string TestCommands::RunConsole(const std::string& line) {
    const auto run=game_.RunConsoleLine(line,true);
    if (!run.dispatched) return run.errorCode+": "+run.errorMessage;
    if (run.capture&&game_.Capture()) game_.Capture()->End(run.capture);
    return {};
}

Json TestCommands::CameraJson(const CameraSnapshot& s) const {
    const bool free=s.mode=="free";
    const auto& active=free?s.free:s.player;
    return {{"mode",s.mode},{"freeCamera",free},{"position",ToJson(active.position)},{"yaw",active.yaw},{"pitch",active.pitch},
        {"fov",{{"world",double(s.worldFov)},{"firstPerson",double(s.firstPersonFov)}}},
        {"cameraPosition",ToJson(s.cameraPosition)},{"player",PoseJson(s.player)},
        {"vanityLocked",vanityLocked_},{"autoVanityAllowed",s.autoVanityAllowed},
        {"note",free?"position/yaw/pitch are the free camera's own pose":
            "position/yaw/pitch are the player's; cameraPosition is where the camera renders from (updates next frame)"}};
}

std::optional<FUSIONMCP_Status> TestCommands::CameraSet(const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
    auto before=world_.SampleCamera();
    if (!before.sampled) return Write(writer,sink,Error("unavailable","Camera state unavailable"));
    std::string reason;
    const auto mode=args.value("mode",before.mode=="free"?std::string("free"):before.mode);
    if (mode!="first"&&mode!="third"&&mode!="free") return Write(writer,sink,Error("invalid_arguments","The current camera mode cannot be set; pass mode"));
    if (mode!=before.mode&&!world_.SetCameraMode(mode,reason)) return Write(writer,sink,Error("camera_failed",reason));
    CameraPose pose=before.mode=="free"?before.free:CameraPose{before.cameraPosition,before.player.yaw,before.player.pitch};
    if (mode!="free") pose=before.player;
    if (args.contains("position")) pose.position=*ParseVec3(args["position"]);
    if (args.contains("yaw")) pose.yaw=args["yaw"].get<double>();
    if (args.contains("pitch")) pose.pitch=args["pitch"].get<double>();
    if (mode=="free") {
        if (!world_.SetFreeCameraPose(pose,reason)) return Write(writer,sink,Error("camera_failed",reason));
    } else {
        if (args.contains("position")) {
            const auto& p=pose.position;
            const auto error=RunConsole("player.SetPos X "+Num(p.x)+";player.SetPos Y "+Num(p.y)+";player.SetPos Z "+Num(p.z));
            if (!error.empty()) return Write(writer,sink,Error("camera_failed","SetPos: "+error));
        }
        if ((args.contains("yaw")||args.contains("pitch"))&&!world_.SetPlayerAngles(pose.yaw,pose.pitch,reason))
            return Write(writer,sink,Error("camera_failed",reason));
    }
    if (args.contains("fov")) {
        const auto& fov=args["fov"];
        const double worldFov=fov.value("world",double(before.worldFov)),first=fov.value("firstPerson",double(before.firstPersonFov));
        const auto error=RunConsole("SetCameraFOV "+Num(worldFov)+" "+Num(first));
        if (!error.empty()) return Write(writer,sink,Error("camera_failed","SetCameraFOV: "+error));
    }
    vanityLocked_=args.value("lockVanity",true);
    world_.SetAutoVanityAllowed(!vanityLocked_);
    auto result=CameraJson(world_.SampleCamera());
    result["requested"]={{"mode",mode},{"position",ToJson(pose.position)},{"yaw",pose.yaw},{"pitch",pose.pitch}};
    return Write(writer,sink,Success(std::move(result)));
}

std::optional<FUSIONMCP_Status> TestCommands::StartCameraPath(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
    for (const auto& [id,o]:operations_) if (o.kind==Kind::Path) return Write(writer,sink,Error("busy","Another camera path is running"));
    const auto s=world_.SampleCamera();
    if (!s.sampled) return Write(writer,sink,Error("unavailable","Camera state unavailable"));
    const CameraPose start=s.mode=="free"?s.free:CameraPose{s.cameraPosition,s.player.yaw,s.player.pitch};
    CameraPath path;
    if (const auto error=ParseCameraPath(args,start,path)) return Write(writer,sink,Error("invalid_arguments",*error));
    const double budget=30000.0+double(args.value("timeoutMs",0u));
    if (path.durationMs+5000>budget)
        return Write(writer,sink,Error("invalid_arguments","The path runs "+Num(path.durationMs)+" ms; pass timeoutMs of at least "+
            Num(path.durationMs+5000-30000)+" so the request outlives it"));
    auto* frames=game_.Frames();
    if (!path.shots.empty()&&(!frames||!frames->Installed()))
        return Write(writer,sink,Error("unavailable","Shots need native frame capture: "+(frames?frames->Detail():std::string("not available"))));
    std::string reason;
    if (s.mode!="free"&&!world_.SetCameraMode("free",reason)) return Write(writer,sink,Error("camera_failed",reason));
    Operation o;o.kind=Kind::Path;o.started=now_();
    o.path=std::make_shared<PathState>(std::move(path));
    const auto state=o.path;const auto now=now_;
    const bool installed=world_.SetCameraDriver([state,now,frames]()->std::optional<CameraPose> {
        std::scoped_lock lock(state->mutex);
        const auto t=now();
        if (!state->first) state->first=t;
        state->last=t;
        const double wall=Ms(*state->first,t);
        const auto step=state->run.Advance(wall);
        for (const auto index:step.due) {
            auto& shot=state->run.Shots()[index];
            const auto admitted=frames?frames->Request({shot.spec.name,nullptr}):FrameCapture::Admission{0,"unavailable","No frame capture"};
            if (admitted.code.empty()) state->run.Requested(index,admitted.id,step,wall);
            else if (admitted.code=="capture_busy") ++shot.busyRetries;
            else state->run.Finished(index,false,Error(admitted.code,admitted.message));
        }
        return step.pose;
    },reason);
    if (!installed) return Write(writer,sink,Error("camera_failed",reason));
    if (args.value("hideHud",false)&&world_.HudVisible()&&world_.SetHudVisible(false)) o.restoreHud=true;
    const auto id="camera-path-"+std::to_string(i.request_id);
    operations_[id]=std::move(o);
    return Pending(writer,sink,id);
}

std::optional<FUSIONMCP_Status> TestCommands::Place(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
    const auto base=world_.ResolveForm(args["base"].get<std::string>());
    if (!base.found) return Write(writer,sink,Error("form_not_found","No loaded form matches base"));
    if (world_.InspectRef(base.formId).found) return Write(writer,sink,Error("invalid_arguments","base must be a base form, not a placed reference"));
    const auto relative=args.value("relativeTo",std::string("player"));
    const auto camera=world_.SampleCamera();
    if (relative!="world"&&!camera.sampled) return Write(writer,sink,Error("unavailable","Player and camera state unavailable"));
    Operation o;o.kind=Kind::Place;o.started=now_();o.base=base.formId;
    if (relative=="world") {
        if (!args.contains("position")) return Write(writer,sink,Error("invalid_arguments","relativeTo world needs position"));
        if (args.contains("offset")) return Write(writer,sink,Error("invalid_arguments","offset applies to player or camera placement"));
        o.target.position=*ParseVec3(args["position"]);
    } else {
        if (args.contains("position")) return Write(writer,sink,Error("invalid_arguments","position applies to relativeTo world; use offset"));
        const bool fromCamera=relative=="camera";
        const CameraPose anchor=fromCamera?(camera.mode=="free"?CameraPose{camera.cameraPosition,camera.free.yaw,camera.free.pitch}:
            CameraPose{camera.cameraPosition,camera.player.yaw,camera.player.pitch}):camera.player;
        const auto offset=args.value("offset",Json::object());
        const Vec3 o3{offset.value("forward",150.0f),offset.value("right",0.0f),offset.value("up",0.0f)};
        o.target.position=OffsetPosition(anchor.position,anchor.yaw,anchor.pitch,o3,fromCamera);
        o.target.yaw=anchor.yaw;
    }
    if (args.contains("yaw")) o.target.yaw=args["yaw"].get<double>();
    o.target.yaw=WrapYaw(o.target.yaw);
    for (const auto& r:world_.Nearby(kPlayerRef,kPlaceSearchRadius,"any",4000)) o.before.insert(r.formId);
    if (const auto error=RunConsole("player.PlaceAtMe "+Hex(base.formId)+" 1");!error.empty())
        return Write(writer,sink,Error("dispatch_failed","PlaceAtMe: "+error));
    if (const auto ref=FindPlaced(o)) if (auto error=MovePlaced(*ref,o)) return Write(writer,sink,*error);
    const auto id="world-place-"+std::to_string(i.request_id);
    operations_[id]=std::move(o);
    return Pending(writer,sink,id);
}
std::optional<std::uint32_t> TestCommands::FindPlaced(const Operation& o) {
    for (const auto& r:world_.Nearby(kPlayerRef,kPlaceSearchRadius,"any",4000))
        if (r.base.formId==o.base&&!o.before.contains(r.formId)) return r.formId;
    return std::nullopt;
}
std::optional<Json> TestCommands::MovePlaced(std::uint32_t ref,Operation& o) {
    const auto id=Hex(ref);
    const auto& p=o.target.position;
    const auto error=RunConsole(id+".SetPos X "+Num(p.x)+";"+id+".SetPos Y "+Num(p.y)+";"+id+".SetPos Z "+Num(p.z)+";"+
        id+".SetAngle Z "+Num(o.target.yaw));
    if (!error.empty()) return Error("move_failed","Placed "+id+" but could not move it: "+error);
    o.kind=Kind::RefSettle;o.ref=ref;o.settlePolls=kRefSettlePolls;
    o.result={{"refId",id},{"requested",{{"position",ToJson(p)},{"yaw",o.target.yaw}}},
        {"note","Placed with PlaceAtMe and moved with SetPos/SetAngle; switch it with fusionmcp.world.ref"}};
    return std::nullopt;
}

std::optional<FUSIONMCP_Status> TestCommands::Invoke(const std::string& command,const FUSIONMCP_InvocationV1& i,const Json& args,
        FUSIONMCP_WriteFn writer,void* sink) {
    if (command=="camera.get") {
        const auto s=world_.SampleCamera();
        return Write(writer,sink,s.sampled?Success(CameraJson(s)):Error("unavailable","Camera state unavailable"));
    }
    if (command=="camera.set") return CameraSet(args,writer,sink);
    if (command=="camera.path") return StartCameraPath(i,args,writer,sink);
    if (command=="capture.stats") {
        if (args.value("source",std::string("backbuffer"))=="f4fs_hdr")
            return Write(writer,sink,Error("unavailable","F4FS has no pre-tonemap HDR readback yet: f4fs.capture has no hdr action "
                "(its PostTonemap set writes 8-bit PNG debug views). Use source backbuffer; transfer pq for an HDR10 swap chain."));
        auto options=std::make_shared<StatsOptions>();
        if (const auto error=ParseStatsOptions(args,*options)) return Write(writer,sink,Error("invalid_arguments",*error));
        auto* frames=game_.Frames();
        if (!frames||!frames->Installed())
            return Write(writer,sink,Error("unavailable","Native frame capture is unavailable: "+(frames?frames->Detail():std::string("no present hook"))));
        Operation o;o.kind=Kind::Stats;o.started=now_();
        o.capture={args.value("label",std::string("stats")),options};
        if (args.value("hideHud",false)&&world_.HudVisible()) {
            if (!world_.SetHudVisible(false)) return Write(writer,sink,Error("capture_failed","HUD could not be hidden"));
            o.restoreHud=true;o.settlePolls=2;
        } else {
            const auto admitted=frames->Request(o.capture);
            if (!admitted.code.empty()) return Write(writer,sink,Error(admitted.code,admitted.message));
            o.frameId=admitted.id;
        }
        const auto id="capture-stats-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    if (command=="perf.sample") {
        auto* frames=game_.Frames();
        if (!frames||!frames->Installed())
            return Write(writer,sink,Error("unavailable","Frame timing reads the present hook, which is unavailable: "+
                (frames?frames->Detail():std::string("no present hook"))));
        Operation o;o.kind=Kind::Perf;o.started=now_();o.durationMs=args.value("durationMs",5000u);o.gpu=args.value("gpuZones",true);
        const auto id="perf-sample-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    if (command=="logs.wait") {
        std::regex pattern;
        try {
            auto flags=std::regex::ECMAScript;
            if (args.value("ignoreCase",false)) flags|=std::regex::icase;
            pattern=std::regex(args["pattern"].get<std::string>(),flags);
        } catch (const std::regex_error&) {return Write(writer,sink,Error("invalid_arguments","pattern is not a valid ECMAScript regular expression"));}
        std::string log=args.value("name",std::string());
        std::optional<std::uint64_t> from;
        if (args.contains("since")) {
            const auto marker=markers_.Find(args["since"].get<std::string>());
            if (!marker) return Write(writer,sink,Error("not_found","since is neither a marker (<log>#<offset>) nor a stored marker id"));
            if (!log.empty()&&!WildcardMatch(log,marker->log)) return Write(writer,sink,Error("invalid_arguments","since marks a different log than name"));
            log=marker->log;from=marker->offset;
        }
        if (log.empty()) return Write(writer,sink,Error("invalid_arguments","Give name or since"));
        Operation o;o.kind=Kind::LogWait;o.started=now_();
        auto& game=game_;
        o.waiter=std::make_unique<LogWaiter>([&game]{return game.ListLogs();},log,std::move(pattern),from,
            std::chrono::milliseconds(args.value("timeoutMs",30000u)));
        const auto id="logs-wait-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    if (command=="game.quit") {
        std::string reason;
        if (!world_.RequestQuit(reason)) return Write(writer,sink,Error("unavailable",reason));
        return Write(writer,sink,Success({{"quitRequested",true},
            {"note","The engine's main loop exits after this frame; later requests may not answer"}}));
    }
    if (command=="world.place") return Place(i,args,writer,sink);
    if (command=="world.ref") {
        const auto form=world_.ResolveForm(args["target"].get<std::string>());
        if (!form.found) return Write(writer,sink,Error("form_not_found","No loaded form matches target"));
        const auto ref=world_.InspectRef(form.formId);
        if (!ref.found) return Write(writer,sink,Error("not_a_reference","The form is not a loaded reference"));
        if (ref.formId==kPlayerRef) return Write(writer,sink,Error("invalid_arguments","The player reference cannot be switched"));
        const auto action=args["action"].get<std::string>();
        const auto id=Hex(ref.formId);
        std::string line=action=="enable"?id+".Enable":id+".Disable";
        if (action=="delete") {
            if ((ref.formId>>24)!=0xFF) return Write(writer,sink,Error("invalid_arguments","delete only removes references created this session (FF...... ids)"));
            line+=";"+id+".MarkForDelete";
        }
        if (const auto error=RunConsole(line);!error.empty()) return Write(writer,sink,Error("dispatch_failed",error));
        Operation o;o.kind=Kind::RefSettle;o.started=now_();o.ref=ref.formId;o.settlePolls=kRefSettlePolls;
        o.result={{"action",action}};
        const auto pending="world-ref-"+std::to_string(i.request_id);
        operations_[pending]=std::move(o);
        return Pending(writer,sink,pending);
    }
    return std::nullopt;
}

void TestCommands::Finish(Operation& o) {
    std::string ignored;
    if (o.kind==Kind::Path) world_.SetCameraDriver({},ignored);
    if (o.frameId&&game_.Frames()) {game_.Frames()->Cancel(o.frameId);o.frameId=0;}
    if (o.waiter) o.waiter->Stop();
    if (o.restoreHud) {world_.SetHudVisible(true);o.restoreHud=false;}
}

std::optional<FUSIONMCP_Status> TestCommands::Poll(const std::string& operation,FUSIONMCP_WriteFn writer,void* sink) {
    const auto it=operations_.find(operation);
    if (it==operations_.end()) return std::nullopt;
    auto& o=it->second;
    const auto now=now_();
    const auto done=[&](const Json& envelope) {
        Finish(o);
        const auto status=Write(writer,sink,envelope);
        operations_.erase(it);
        return status;
    };
    auto* frames=game_.Frames();
    switch (o.kind) {
    case Kind::Path: {
        auto& state=*o.path;
        std::scoped_lock lock(state.mutex);
        for (std::size_t n=0;n<state.run.Shots().size();++n) {
            auto& shot=state.run.Shots()[n];
            if (shot.state!=CameraPathRun::ShotState::Requested||!frames) continue;
            const auto status=frames->Poll(shot.captureId);
            if (status.state==FrameCapture::State::Completed) state.run.Finished(n,true,ArtifactJson(status));
            else if (status.state==FrameCapture::State::Failed) state.run.Finished(n,false,Error(status.code,status.message));
        }
        if (!state.first&&now-o.started>=kStallLimit)
            return done(Error("camera_stalled","The free camera never updated: the game is paused, minimized or not rendering"));
        if (state.first&&!state.run.MotionDone()&&now-state.last>=kStallLimit)
            return done(Error("camera_stalled","The free camera stopped updating mid-path (it left free mode, or the game paused)"));
        if (!state.run.MotionDone()||!state.run.ShotsSettled()) return FUSIONMCP_PENDING;
        Json shots=Json::array();
        bool allOk=true;
        for (const auto& shot:state.run.Shots()) {
            const bool ok=shot.state==CameraPathRun::ShotState::Done;
            allOk&=ok;
            Json entry={{"name",shot.spec.name},{"atMs",shot.spec.atMs},{"takenAtPathMs",shot.takenAtPathMs},
                {"takenAtWallMs",std::round(shot.takenAtWallMs*10)/10},{"frame",shot.frame},{"busyRetries",shot.busyRetries},{"ok",ok}};
            entry[ok?"artifact":"error"]=ok?shot.result:shot.result.value("error",Json());
            shots.push_back(std::move(entry));
        }
        const auto& path=state.run.Path();
        return done(Success({{"durationMs",path.durationMs},{"wallMs",std::round(state.run.LastWallMs()*10)/10},
            {"frames",state.run.Frames()},{"timing",path.frameStepMs?"fixed_step":"realtime"},
            {"frameTimes",ToJson(Summarize(state.run.FrameTimes()))},{"finalPose",PoseJson(path.At(path.durationMs))},
            {"shots",std::move(shots)},{"allShotsOk",allOk},{"mode","free"},
            {"note","The camera stays in free mode at the final pose; fusionmcp.camera.set restores a saved pose"}}));
    }
    case Kind::Stats: {
        if (o.settlePolls) {
            if (--o.settlePolls) return FUSIONMCP_PENDING;
            const auto admitted=frames?frames->Request(o.capture):FrameCapture::Admission{0,"unavailable","No frame capture"};
            if (!admitted.code.empty()) return done(Error(admitted.code,admitted.message));
            o.frameId=admitted.id;
            return FUSIONMCP_PENDING;
        }
        const auto status=frames->Poll(o.frameId);
        if (status.state==FrameCapture::State::Pending) return FUSIONMCP_PENDING;
        o.frameId=0;
        if (status.state==FrameCapture::State::Failed) return done(Error(status.code,status.message));
        auto result=status.stats?ToJson(*status.stats):Json::object();
        result["artifact"]=ArtifactJson(status);result["source"]="native_present_backbuffer";
        result["swapChainFormat"]=status.dxgiFormat;result["presentCount"]=status.presentCount;result["hudHidden"]=o.restoreHud;
        result["note"]="Measured on the exact frame saved as artifact.name (display-referred, post-tonemap, HUD included unless hidden)";
        return done(Success(std::move(result)));
    }
    case Kind::Perf: {
        if (!o.job) {
            if (Ms(o.started,now)<double(o.durationMs)) return FUSIONMCP_PENDING;
            const auto presents=frames?frames->PresentsSince(o.started):std::vector<RequestTime>{};
            std::vector<double> intervals;
            for (std::size_t n=1;n<presents.size();++n) intervals.push_back(Ms(presents[n-1],presents[n]));
            const auto stats=Summarize(intervals);
            const double seconds=Ms(o.started,now)/1000.0;
            o.result={{"durationMs",std::round(Ms(o.started,now))},{"source","present_intervals"},{"frameTime",ToJson(stats)},
                {"fps",seconds>0?std::round(double(stats.count)/seconds*100)/100:0.0},
                {"note","Intervals between the game's Present calls on its swap chain (CPU-side frame pacing)"}};
            o.job=std::make_shared<GpuJob>();
            if (!o.gpu) {o.job->result={{"available",false},{"reason","Not requested (gpuZones=false)"}};o.job->done=true;}
            else if (!f4fs_) {o.job->result={{"available",false},{"reason","F4FS GPU zones are not wired into this host"}};o.job->done=true;}
            else std::thread([job=o.job,call=f4fs_] {
                Json gpu;
                try {
                    const auto r=call("f4fs.inspect",{{"kind","perf"}});
                    if (r.value("ok",false)&&r["result"].is_object()) {
                        gpu=r["result"];
                        if (!gpu.contains("available")) gpu["available"]=true;
                    } else {
                        const auto code=r.value("error",Json::object()).value("code",std::string("unavailable"));
                        const auto message=r.value("error",Json::object()).value("message",std::string());
                        gpu={{"available",false},{"reason",code=="unavailable"?message:
                            "This F4FS build does not answer f4fs.inspect kind=perf (added with F4FS remote settings): "+code+": "+message}};
                    }
                } catch (...) {gpu={{"available",false},{"reason","F4FS GPU zone query failed"}};}
                job->result=std::move(gpu);
                job->done.store(true,std::memory_order_release);
            }).detach();
        }
        if (!o.job->done.load(std::memory_order_acquire)) return FUSIONMCP_PENDING;
        o.result["gpu"]=o.job->result;
        return done(Success(o.result));
    }
    case Kind::LogWait: {
        const auto r=o.waiter->Poll();
        using State=LogWaiter::Result::State;
        if (r.state==State::Waiting) return FUSIONMCP_PENDING;
        if (r.state==State::Failed) return done(Error("not_found",r.error));
        if (r.state==State::TimedOut) return done(Error("timeout","No line matching the pattern was appended to "+r.log+" within the "
            "timeout (scanned "+std::to_string(r.scanned)+" bytes from offset "+std::to_string(r.startOffset)+"; resume with since "+
            FormatMarker({r.log,r.nextOffset})+")"));
        return done(Success({{"log",r.log},{"matched",true},{"line",Printable(r.line)},{"lineOffset",r.lineOffset},
            {"nextOffset",r.nextOffset},{"marker",FormatMarker({r.log,r.nextOffset})},{"startOffset",r.startOffset},
            {"scannedBytes",r.scanned},{"rotated",r.rotated},{"elapsedMs",std::round(Ms(o.started,now))}}));
    }
    case Kind::Place: {
        if (const auto ref=FindPlaced(o)) {
            if (auto error=MovePlaced(*ref,o)) return done(*error);
            return FUSIONMCP_PENDING;
        }
        if (now-o.started>=kPlaceWindow) return done(Error("spawn_failed","No new reference of the base form appeared near the player"));
        return FUSIONMCP_PENDING;
    }
    case Kind::RefSettle: {
        if (o.settlePolls) {--o.settlePolls;return FUSIONMCP_PENDING;}
        auto result=o.result;
        result["ref"]=RefJson(world_.InspectRef(o.ref));
        return done(Success(std::move(result)));
    }
    }
    return FUSIONMCP_INTERNAL_FAILURE;
}

bool TestCommands::Cancel(const std::string& operation) {
    const auto it=operations_.find(operation);
    if (it==operations_.end()) return false;
    Finish(it->second);
    operations_.erase(it);
    return true;
}

void TestCommands::SampleOnGame() {
    if (vanityLocked_) world_.SetAutoVanityAllowed(false);
}
}
