#include "WorldCommands.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace fusionmcp {
namespace {
constexpr const char* kTargetDescription="\"player\", 8 hex digits (runtime FormID), \"ObjectID:Plugin.esm\" or an EditorID";
constexpr const char* kResolveSchema=R"({"type":"object","properties":{"form":{"type":"string","minLength":1,"maxLength":128}},"required":["form"],"additionalProperties":false})";
constexpr const char* kTargetSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128}},"required":["target"],"additionalProperties":false})";
constexpr const char* kActorSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128},
"inventoryLimit":{"type":"integer","minimum":0,"maximum":200}},"required":["target"],"additionalProperties":false})";
constexpr const char* kNearbySchema=R"({"type":"object","properties":{"anchor":{"type":"string","minLength":1,"maxLength":128},
"radius":{"type":"number","minimum":1,"maximum":50000},"kind":{"type":"string","minLength":1,"maxLength":16},
"limit":{"type":"integer","minimum":1,"maximum":100}},"additionalProperties":false})";
constexpr const char* kSpawnSchema=R"({"type":"object","properties":{"base":{"type":"string","minLength":1,"maxLength":128},
"anchor":{"type":"string","minLength":1,"maxLength":128},"count":{"type":"integer","minimum":1,"maximum":20}},"required":["base"],"additionalProperties":false})";
constexpr const char* kEmpty=R"({"type":"object","properties":{},"additionalProperties":false})";
constexpr const char* kGlobalSchema=R"({"type":"object","properties":{"global":{"type":"string","minLength":1,"maxLength":128,
"description":"Global variable (GLOB) by EditorID such as GameHour, 8 hex digits (runtime FormID) or ObjectID:Plugin.esm"}},
"required":["global"],"additionalProperties":false})";
constexpr const char* kGlobalSetSchema=R"({"type":"object","properties":{"global":{"type":"string","minLength":1,"maxLength":128},
"value":{"type":"number"}},"required":["global","value"],"additionalProperties":false})";
constexpr const char* kTimeSchema=R"({"type":"object","properties":{"hour":{"type":"number","minimum":0,"maximum":24,
"description":"Hour of day, 0 to below 24; 12.5 is 12:30"}},"required":["hour"],"additionalProperties":false})";
constexpr const char* kAnswerSchema=R"({"type":"object","properties":{"button":{"type":"integer","minimum":0,"maximum":31},
"label":{"type":"string","minLength":1,"maxLength":128}},"additionalProperties":false})";
constexpr const char* kModeSchema=R"({"type":"object","properties":{"mode":{"enum":["first","third","free"]}},"required":["mode"],"additionalProperties":false})";
constexpr const char* kLookSchema=R"({"type":"object","properties":{"yaw":{"type":"number","minimum":-360,"maximum":360},
"pitch":{"type":"number","minimum":-89,"maximum":89},"relative":{"type":"boolean"},
"target":{"type":"string","minLength":1,"maxLength":128}},"additionalProperties":false})";
constexpr const char* kHoldSchema=R"({"type":"object","properties":{"controls":{"type":"array","minItems":1,"maxItems":6,"items":{"enum":
["forward","back","strafe_left","strafe_right","jump","sprint","sneak","activate","reload","fire","aim","ready_weapon"]}},
"durationMs":{"type":"integer","minimum":1,"maximum":10000}},"required":["controls","durationMs"],"additionalProperties":false})";
constexpr const char* kWaitSchema=R"({"type":"object","properties":{"condition":{"enum":["world_ready","menu_open","menu_closed",
"messagebox_open","messagebox_closed","ref_3d","in_combat","not_in_combat","dead","near_player"]},
"target":{"type":"string","minLength":1,"maxLength":128},"menu":{"type":"string","minLength":1,"maxLength":64},
"radius":{"type":"number","minimum":1,"maximum":50000},"timeoutMs":{"type":"integer","minimum":1,"maximum":180000}},
"required":["condition"],"additionalProperties":false})";
constexpr const char* kVariablesSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128},
"names":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"string","minLength":1,"maxLength":64}}},
"required":["target","names"],"additionalProperties":false})";
constexpr const char* kWatchSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128},
"durationMs":{"type":"integer","minimum":1,"maximum":30000},"contains":{"type":"string","minLength":1,"maxLength":64}},
"required":["target","durationMs"],"additionalProperties":false})";
constexpr const char* kNodeTransformSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128,"description":"Default player"},
"perspective":{"enum":["1p","3p"],"description":"3D root: 1p = Get3D(true) (player first-person skeleton), 3p = Get3D(false). Default 3p"},
"node":{"type":"string","minLength":1,"maxLength":128,"description":"Node name, e.g. Weapon, RArm_Hand, LArm_Hand, Camera"}},
"required":["node"],"additionalProperties":false})";
constexpr const char* kNodeChildrenSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128,"description":"Default player"},
"perspective":{"enum":["1p","3p"]},"node":{"type":"string","minLength":1,"maxLength":128,"description":"Subtree root; default the 3D root"},
"depth":{"type":"integer","minimum":0,"maximum":16,"description":"Levels below the start node, default 2"},
"limit":{"type":"integer","minimum":1,"maximum":500,"description":"Max entries, default 200"}},"additionalProperties":false})";
constexpr const char* kActiveClipsSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128,"description":"Default player"},
"limit":{"type":"integer","minimum":1,"maximum":128,"description":"Max clips, default 48"}},"additionalProperties":false})";
constexpr const char* kWeapon3DSchema=R"({"type":"object","properties":{"target":{"type":"string","minLength":1,"maxLength":128,"description":"Default player"}},
"additionalProperties":false})";
constexpr const char* kMatrixConvention="rotate[row][col] is NiMatrix3::entry as stored. FO4 renders child = transpose(rotate) * point "
    "(the transpose of CommonLib's NiMatrix3 operator*): row r of rotate is the parent-space image of local axis r.";

std::string Printable(std::string text) {
    for (char& c:text) {const auto u=static_cast<unsigned char>(c);if (u<32||u>126) c='?';}
    return text;
}
std::string MultilineText(std::string text) {
    for (char& c:text) {const auto u=static_cast<unsigned char>(c);if ((u<32&&c!='\n')||u>126) c='?';}
    return text;
}
std::string Lower(std::string text) {
    for (char& c:text) if (c>='A'&&c<='Z') c=static_cast<char>(c+('a'-'A'));
    return text;
}
// Engine boxes can show untranslated "$Key" button labels; accept the label with or without the "$".
bool LabelMatches(const std::string& button,const std::string& wanted) {
    const auto label=Lower(button);
    return label==wanted||(label.size()>1&&label[0]=='$'&&label.substr(1)==wanted);
}
std::string Hex(std::uint32_t value,int digits) {
    char buffer[16];std::snprintf(buffer,sizeof(buffer),"%0*X",digits,value);return buffer;
}
double Round(double value) {return std::round(value*100.0)/100.0;}
double Precise(double value) {return std::round(value*1.0e5)/1.0e5;}
Json PreciseVec(const Vec3& v) {return Json::array({Precise(v.x),Precise(v.y),Precise(v.z)});}
Json ToJson(const NodeTransform& t) {
    Json rows=Json::array();
    for (const auto& row:t.rotate) rows.push_back(Json::array({Precise(row[0]),Precise(row[1]),Precise(row[2])}));
    return {{"translate",PreciseVec(t.translate)},{"rotate",std::move(rows)},{"scale",Precise(t.scale)}};
}
Json Bound(const Vec3& center,float radius) {return {{"center",PreciseVec(center)},{"radius",Precise(radius)}};}
std::string Flags(std::uint64_t flags) {return Hex(static_cast<std::uint32_t>(flags>>32),8)+Hex(static_cast<std::uint32_t>(flags),8);}
Json ToJson(const Vec3& v) {return {{"x",Round(v.x)},{"y",Round(v.y)},{"z",Round(v.z)}};}
Json ToJson(const FormInfo& f) {
    Json j={{"formId",Hex(f.formId,8)},{"type",Printable(f.formType)},{"editorId",Printable(f.editorId)},{"name",Printable(f.name)}};
    if (!f.plugin.empty()) {j["plugin"]=Printable(f.plugin);j["formKey"]=Hex(f.localId,6)+":"+Printable(f.plugin);}
    return j;
}
Json ToJson(const RefInfo& r) {
    Json j={{"refId",Hex(r.formId,8)},{"base",ToJson(r.base)},{"name",Printable(r.name)},{"cell",Printable(r.cell)},
        {"worldspace",Printable(r.worldspace)},{"position",ToJson(r.position)},{"angle",ToJson(r.angle)},{"enabled",r.enabled},
        {"deleted",r.deleted},{"has3D",r.has3D},{"isActor",r.isActor}};
    if (r.distanceToPlayer>=0) j["distanceToPlayer"]=Round(r.distanceToPlayer);
    return j;
}
const char* ControlName(Control c) {
    static constexpr const char* names[]={"forward","back","strafe_left","strafe_right","jump","sprint","sneak","activate","reload",
        "fire","aim","ready_weapon"};
    return names[static_cast<int>(c)];
}
std::optional<Control> ParseControl(const std::string& name) {
    for (int i=0;i<=static_cast<int>(Control::ReadyWeapon);++i) if (name==ControlName(static_cast<Control>(i))) return static_cast<Control>(i);
    return std::nullopt;
}
FUSIONMCP_Status Write(FUSIONMCP_WriteFn writer,void* sink,const Json& envelope) {
    const auto bytes=envelope.dump();
    if (bytes.size()>FUSIONMCP_MAX_RESULT_BYTES) return FUSIONMCP_INTERNAL_FAILURE;
    return writer(sink,bytes.data(),static_cast<uint32_t>(bytes.size()))?FUSIONMCP_OK:FUSIONMCP_WRITER_FAILED;
}
FUSIONMCP_Status Pending(FUSIONMCP_WriteFn writer,void* sink,const std::string& id) {
    return writer(sink,id.data(),static_cast<uint32_t>(id.size()))?FUSIONMCP_PENDING:FUSIONMCP_WRITER_FAILED;
}
std::int64_t Ms(RequestTime from,RequestTime to) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(to-from).count();
}
}

std::vector<WorldCommands::Spec> WorldCommands::Specs() {
    constexpr uint32_t mutating=FUSIONMCP_COMMAND_MUTATING,async=FUSIONMCP_COMMAND_ASYNC;
    return {
        {"forms.resolve",std::string("Resolve a form by ")+kTargetDescription+" to its runtime FormID, FormKey, type, EditorID and name.",kResolveSchema,0},
        {"global.get","Read a global variable (GLOB) directly: value, FormID, EditorID and whether it is constant.",kGlobalSchema,0},
        {"global.set","Write a global variable (GLOB) directly, without the script compiler; returns the previous and new value. "
            "Constant globals are refused, as the console refuses them.",kGlobalSetSchema,mutating},
        {"time.set","Set the hour of day (the Calendar's GameHour global) directly; returns the previous and new hour.",kTimeSchema,mutating},
        {"ref.inspect","Copy of one loaded reference: base form, position, angle (degrees), cell/worldspace, enabled, 3D loaded, distance to player.",kTargetSchema,0},
        {"actor.state","Actor reference state: dead, health, combat and target, AI package, race, level and inventory with equipped flags (bounded).",kActorSchema,0},
        {"scene.nearby","Loaded references near an anchor (default player), nearest first. kind: any (default), actor, or a form type such as WEAP or CONT.",kNearbySchema,0},
        {"world.spawn","Place 1..20 references of a base form at an anchor (default player). "
            "Async: runs the engine PlaceAtMe and returns the new reference ids found near the anchor.",kSpawnSchema,mutating|async},
        {"ui.menus","Open menu names and the visible message box (text and button labels), if any.",kEmpty,0},
        {"ui.messagebox.answer","Choose a button of the visible engine message box by index or exact label (case-insensitive).",kAnswerSchema,mutating},
        {"camera.state","Camera mode (first, third, free) and player yaw/pitch in degrees.",kEmpty,0},
        {"camera.mode","Switch the camera to first person, third person, or free camera (simulation keeps running).",kModeSchema,mutating},
        {"camera.look","Set player yaw/pitch in degrees (absolute, or relative=true), or turn to face target.",kLookSchema,mutating},
        {"input.hold","Hold player controls for durationMs (max 10 s) through the engine input path, then release. Async; cancel releases.",kHoldSchema,mutating|async},
        {"wait.until","Async barrier: completes when the condition holds on two consecutive Game owner samples, or fails at timeoutMs "
            "(default 30 s). Conditions: world_ready, menu_open/menu_closed (menu), messagebox_open/closed, ref_3d, in_combat, "
            "not_in_combat, dead (target), near_player (target, radius).",kWaitSchema,async},
        {"anim.variables","Read animation graph variables (float, int or bool) on a reference.",kVariablesSchema,0},
        {"node.transform","Read-only: one scene graph node of a reference's 1P or 3P 3D by name: local and world transform "
            "(translate, 3x3 rotate as stored, scale), parent, flags, appCulled / ancestorCulled and world bound.",kNodeTransformSchema,0},
        {"node.children","Read-only: depth-limited subtree under a named node or the 3D root: name, RTTI type, flags, appCulled, "
            "local translate and child count per node, depth-first.",kNodeChildrenSchema,0},
        {"anim.active_clips","Read-only: clip generators on the active branch of every behaviour graph of a reference, and for actors "
            "the equipped weapon's 1P/3P graphs: node, animation file, behaviour file, playback speed, local time.",kActiveClipsSchema,0},
        {"actor.equipped_weapon_3d","Read-only: the equipped weapon's biped 3D for 1P and 3P: loaded, attached, parent node, world "
            "transform, world bound and app-culled state of it or an ancestor.",kWeapon3DSchema,0},
        {"actor.weapon_cull_state","Read-only: why an actor's weapon may be culled: weaponCullCounter (raw and decoded), weaponState, "
            "gunState, the AI process weapon-loading queries, Weapon / RArm_Hand culling in 1P and 3P and the weapon part's graph manager.",
            kWeapon3DSchema,0},
        {"anim.watch","Record animation graph events of a reference for durationMs (max 30 s), optionally only tags containing a substring.",kWatchSchema,async},
    };
}

WorldCommands::WorldCommands(WorldServices& world,GameServices& game,RequestBroker::NowFn now):world_(world),game_(game),now_(std::move(now)) {}
WorldCommands::~WorldCommands()=default;

std::optional<std::uint32_t> WorldCommands::Target(const Json& args,const char* key,const char* fallback,Json& error) {
    const auto spec=args.contains(key)?args[key].get<std::string>():std::string(fallback?fallback:"");
    if (spec.empty()) {error=Error("invalid_arguments",std::string(key)+" is required");return std::nullopt;}
    const auto form=world_.ResolveForm(spec);
    if (!form.found) {error=Error("form_not_found","No loaded form matches "+Printable(spec));return std::nullopt;}
    return form.formId;
}

std::optional<FUSIONMCP_Status> WorldCommands::Invoke(const std::string& command,const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
    Json error;
    const auto reference=[&](const char* key,const char* fallback)->std::optional<RefInfo> {
        const auto id=Target(args,key,fallback,error);
        if (!id) return std::nullopt;
        auto ref=world_.InspectRef(*id);
        if (!ref.found) {error=Error("not_a_reference","The form is not a loaded reference");return std::nullopt;}
        return ref;
    };
    if (command=="forms.resolve") {
        const auto form=world_.ResolveForm(args["form"].get<std::string>());
        return Write(writer,sink,form.found?Success(ToJson(form)):Error("form_not_found","No loaded form matches"));
    }
    if (command=="global.get"||command=="global.set"||command=="time.set") {
        std::uint32_t id=0;
        if (command=="time.set") {
            id=world_.GameHourGlobal();
            if (!id) return Write(writer,sink,Error("unavailable","The calendar's GameHour global is unavailable"));
        } else {
            const auto form=world_.ResolveForm(args["global"].get<std::string>());
            if (!form.found) return Write(writer,sink,Error("form_not_found","No loaded form matches global"));
            id=form.formId;
        }
        const auto before=world_.ReadGlobal(id);
        if (!before.found) return Write(writer,sink,Error("not_a_global","The form is not a global variable (GLOB)"));
        Json result={{"global",ToJson(before.form)},{"constant",before.constant}};
        if (command=="global.get") {result["value"]=before.value;return Write(writer,sink,Success(std::move(result)));}
        if (before.constant) return Write(writer,sink,Error("global_constant","The global is flagged constant; the engine refuses writes to it"));
        const auto value=command=="time.set"?args["hour"].get<double>():args["value"].get<double>();
        if (!std::isfinite(value)||std::fabs(value)>3.0e38) return Write(writer,sink,Error("invalid_arguments","value must be a finite float"));
        if (command=="time.set"&&value>=24) return Write(writer,sink,Error("invalid_arguments","hour must be below 24"));
        std::string reason;
        if (!world_.WriteGlobal(id,static_cast<float>(value),reason)) return Write(writer,sink,Error("write_failed",reason));
        result["previous"]=before.value;result["value"]=world_.ReadGlobal(id).value;
        return Write(writer,sink,Success(std::move(result)));
    }
    if (command=="ref.inspect") {
        const auto ref=reference("target",nullptr);
        return Write(writer,sink,ref?Success(ToJson(*ref)):error);
    }
    if (command=="actor.state") {
        const auto ref=reference("target",nullptr);
        if (!ref) return Write(writer,sink,error);
        if (!ref->isActor) return Write(writer,sink,Error("not_an_actor","The reference is not an actor"));
        const auto actor=world_.InspectActor(ref->formId,args.value("inventoryLimit",50u));
        Json inventory=Json::array();
        for (const auto& stack:actor.inventory) inventory.push_back({{"item",ToJson(stack.item)},{"count",stack.count},{"equipped",stack.equipped}});
        return Write(writer,sink,Success({{"ref",ToJson(actor.ref)},{"dead",actor.dead},{"inCombat",actor.inCombat},
            {"health",Round(actor.health)},{"healthBase",Round(actor.healthBase)},
            {"combatTarget",actor.combatTarget?Json(Hex(actor.combatTarget,8)):Json()},{"package",Printable(actor.package)},
            {"race",ToJson(actor.race)},{"level",actor.level},{"inventory",std::move(inventory)},{"inventoryTruncated",actor.inventoryTruncated}}));
    }
    if (command=="scene.nearby") {
        const auto anchor=Target(args,"anchor","player",error);
        if (!anchor) return Write(writer,sink,error);
        Json refs=Json::array();
        for (const auto& r:world_.Nearby(*anchor,args.value("radius",2000.0f),Lower(args.value("kind",std::string("any"))),args.value("limit",25u)))
            refs.push_back(ToJson(r));
        return Write(writer,sink,Success({{"anchor",Hex(*anchor,8)},{"references",std::move(refs)}}));
    }
    if (command=="world.spawn") {
        const auto base=Target(args,"base",nullptr,error);
        const auto anchor=base?Target(args,"anchor","player",error):std::nullopt;
        if (!anchor) return Write(writer,sink,error);
        const auto anchorRef=world_.InspectRef(*anchor);
        if (!anchorRef.found) return Write(writer,sink,Error("not_a_reference","anchor is not a loaded reference"));
        if (world_.InspectRef(*base).found) return Write(writer,sink,Error("invalid_arguments","base must be a base form, not a placed reference"));
        Operation o;o.kind=Kind::Spawn;o.started=now_();o.deadline=o.started+kSpawnWindow;
        o.target=*anchor;o.base=*base;o.count=args.value("count",1u);
        o.radius=kSpawnSearchRadius;
        for (const auto& r:world_.Nearby(*anchor,o.radius,"any",kSpawnScanLimit)) o.before.insert(r.formId);
        // The engine's own PlaceAtMe. Its optional distance/direction arguments placed nothing on 1.11.240.
        const auto line=(*anchor==kPlayerRef?std::string("player"):Hex(*anchor,8))+".PlaceAtMe "+Hex(*base,8)+" "+std::to_string(o.count);
        if (!game_.ExecuteConsole(line)) return Write(writer,sink,Error("dispatch_failed","Native console refused the PlaceAtMe line"));
        const auto id="world-spawn-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    if (command=="ui.menus") {
        Json menus=Json::array();
        for (auto& name:world_.OpenMenus()) menus.push_back(Printable(name));
        const auto box=world_.ReadMessageBox();
        Json buttons=Json::array();
        for (const auto& b:box.buttons) buttons.push_back(Printable(b));
        return Write(writer,sink,Success({{"open",std::move(menus)},
            {"messageBox",box.open?Json{{"text",MultilineText(box.text)},{"buttons",std::move(buttons)}}:Json()}}));
    }
    if (command=="ui.messagebox.answer") {
        const auto box=world_.ReadMessageBox();
        if (!box.open) return Write(writer,sink,Error("no_messagebox","No engine message box is visible"));
        std::optional<std::uint32_t> button;
        if (args.contains("button")) button=args["button"].get<std::uint32_t>();
        else if (args.contains("label")) {
            const auto wanted=Lower(args["label"].get<std::string>());
            for (std::size_t b=0;b<box.buttons.size();++b) if (LabelMatches(box.buttons[b],wanted)) button=static_cast<std::uint32_t>(b);
        }
        if (!button||*button>=box.buttons.size()) return Write(writer,sink,Error("invalid_arguments","Give button (index) or label matching a visible button"));
        std::string reason;
        if (!world_.AnswerMessageBox(*button,reason)) return Write(writer,sink,Error("answer_failed",reason));
        return Write(writer,sink,Success({{"answered",*button},{"label",Printable(box.buttons[*button])},{"text",MultilineText(box.text)}}));
    }
    if (command=="camera.state") {
        const auto c=world_.Camera();
        if (!c.sampled) return Write(writer,sink,Error("unavailable","Camera state unavailable"));
        return Write(writer,sink,Success({{"mode",c.mode},{"yaw",Round(c.yaw)},{"pitch",Round(c.pitch)}}));
    }
    if (command=="camera.mode") {
        std::string reason;
        if (!world_.SetCameraMode(args["mode"].get<std::string>(),reason)) return Write(writer,sink,Error("camera_failed",reason));
        const auto c=world_.Camera();
        return Write(writer,sink,Success({{"requested",args["mode"]},{"mode",c.mode},
            {"note","Camera transitions finish over the next frames; check camera.state or a screenshot."}}));
    }
    if (command=="camera.look") {
        std::string reason;bool ok=false;
        if (args.contains("target")) {
            if (args.contains("yaw")||args.contains("pitch")) return Write(writer,sink,Error("invalid_arguments","Give target or yaw/pitch, not both"));
            const auto id=Target(args,"target",nullptr,error);
            if (!id) return Write(writer,sink,error);
            ok=world_.FaceReference(*id,reason);
        } else {
            if (!args.contains("yaw")&&!args.contains("pitch")) return Write(writer,sink,Error("invalid_arguments","Give yaw, pitch or target"));
            const bool relative=args.value("relative",false);
            const auto now=world_.Camera();
            ok=world_.SetLook(args.value("yaw",relative?0.0f:now.yaw),args.value("pitch",relative?0.0f:now.pitch),relative,reason);
        }
        if (!ok) return Write(writer,sink,Error("camera_failed",reason));
        const auto c=world_.Camera();
        return Write(writer,sink,Success({{"yaw",Round(c.yaw)},{"pitch",Round(c.pitch)},{"mode",c.mode}}));
    }
    if (command=="input.hold") {
        Operation o;o.kind=Kind::Hold;o.started=now_();
        o.deadline=o.started+std::chrono::milliseconds(args["durationMs"].get<std::int64_t>());
        for (const auto& name:args["controls"]) o.controls.push_back(*ParseControl(name.get<std::string>()));
        for (const auto c:o.controls) {
            std::string reason;
            if (!world_.SetControl(c,true,reason)) {
                for (const auto held:o.controls) world_.SetControl(held,false,reason);
                return Write(writer,sink,Error("input_failed",std::string(ControlName(c))+": "+reason));
            }
        }
        const auto id="world-hold-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    if (command=="wait.until") {
        Operation o;o.kind=Kind::Wait;o.started=now_();
        o.deadline=o.started+std::chrono::milliseconds(args.value("timeoutMs",static_cast<std::int64_t>(kDefaultWait.count())));
        o.condition=args["condition"].get<std::string>();
        const bool needsTarget=o.condition=="ref_3d"||o.condition=="in_combat"||o.condition=="not_in_combat"||o.condition=="dead"||o.condition=="near_player";
        const bool needsMenu=o.condition=="menu_open"||o.condition=="menu_closed";
        if (needsTarget) {
            const auto id=Target(args,"target",nullptr,error);
            if (!id) return Write(writer,sink,error);
            o.target=*id;
        }
        if (needsMenu) {
            if (!args.contains("menu")) return Write(writer,sink,Error("invalid_arguments","menu is required for "+o.condition));
            o.menu=args["menu"].get<std::string>();
        }
        if (o.condition=="near_player") o.radius=args.value("radius",500.0f);
        const auto id="world-wait-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    if (command=="anim.variables") {
        const auto ref=reference("target",nullptr);
        if (!ref) return Write(writer,sink,error);
        std::vector<std::string> names;
        for (const auto& n:args["names"]) names.push_back(n.get<std::string>());
        std::string reason;
        const auto values=world_.GraphVariables(ref->formId,names,reason);
        if (!reason.empty()&&values.empty()) return Write(writer,sink,Error("graph_unavailable",reason));
        Json out=Json::object();
        for (const auto& v:values) {
            Json typed=Json::object();
            if (v.hasFloat) typed["float"]=v.asFloat;
            if (v.hasInt) typed["int"]=v.asInt;
            if (v.hasBool) typed["bool"]=v.asBool;
            out[Printable(v.name)]=typed.empty()?Json():std::move(typed);
        }
        return Write(writer,sink,Success({{"refId",Hex(ref->formId,8)},{"variables",std::move(out)}}));
    }
    if (command=="node.transform"||command=="node.children") {
        const auto ref=reference("target","player");
        if (!ref) return Write(writer,sink,error);
        if (!ref->has3D) return Write(writer,sink,Error("no_3d","The reference has no loaded 3D"));
        const auto perspective=args.value("perspective",std::string("3p"));
        const bool firstPerson=perspective=="1p";
        const auto name=args.value("node",std::string());
        std::string reason;
        if (command=="node.transform") {
            const auto n=world_.Node(ref->formId,firstPerson,name,reason);
            if (!n) return Write(writer,sink,Error("node_not_found",reason));
            return Write(writer,sink,Success({{"refId",Hex(ref->formId,8)},{"perspective",perspective},{"node",Printable(n->name)},
                {"type",Printable(n->type)},{"parent",n->parent.empty()?Json():Json(Printable(n->parent))},
                {"local",ToJson(n->local)},{"world",ToJson(n->world)},{"worldBound",Bound(n->boundCenter,n->boundRadius)},
                {"flags",Flags(n->flags)},{"appCulled",n->appCulled},{"ancestorCulled",n->ancestorCulled},{"matrixConvention",kMatrixConvention}}));
        }
        bool truncated=false;
        const auto nodes=world_.NodeChildren(ref->formId,firstPerson,name,args.value("depth",2u),args.value("limit",200u),truncated,reason);
        if (nodes.empty()) return Write(writer,sink,Error("node_not_found",reason));
        Json out=Json::array();
        for (const auto& n:nodes) out.push_back({{"depth",n.depth},{"name",Printable(n.name)},{"type",Printable(n.type)},
            {"flags",Flags(n.flags)},{"appCulled",n.appCulled},{"localTranslate",PreciseVec(n.localTranslate)},{"children",n.childCount}});
        return Write(writer,sink,Success({{"refId",Hex(ref->formId,8)},{"perspective",perspective},{"nodes",std::move(out)},{"truncated",truncated}}));
    }
    if (command=="anim.active_clips") {
        const auto ref=reference("target","player");
        if (!ref) return Write(writer,sink,error);
        bool truncated=false;std::string reason;
        const auto clips=world_.ActiveClips(ref->formId,args.value("limit",48u),truncated,reason);
        if (clips.empty()&&!reason.empty()) return Write(writer,sink,Error("graph_unavailable",reason));
        Json out=Json::array();
        for (const auto& c:clips) out.push_back({{"source",c.source},{"graphIndex",c.graphIndex},{"activeGraph",c.activeGraph},{"graph",Printable(c.graph)},
            {"node",Printable(c.node)},{"animation",Printable(c.animation)},{"behavior",c.behavior.empty()?Json():Json(Printable(c.behavior))},
            {"playbackSpeed",Precise(c.playbackSpeed)},{"localTime",c.hasControl?Json(Precise(c.localTime)):Json()},
            {"controlWeight",c.hasControl?Json(Precise(c.controlWeight)):Json()}});
        return Write(writer,sink,Success({{"refId",Hex(ref->formId,8)},{"clips",std::move(out)},{"truncated",truncated},
            {"note","localTime and controlWeight are the clip's hkaDefaultAnimationControl values; controlWeight is not the blend tree's resolved weight."}}));
    }
    if (command=="actor.equipped_weapon_3d") {
        const auto ref=reference("target","player");
        if (!ref) return Write(writer,sink,error);
        if (!ref->isActor) return Write(writer,sink,Error("not_an_actor","The reference is not an actor"));
        const auto view=[&](bool firstPerson) {
            const auto w=world_.EquippedWeapon3D(ref->formId,firstPerson);
            if (!w.biped) return Json();
            Json j={{"slot",w.slot>=0?Json(w.slot):Json()},{"weapon",w.slot>=0?Json(Hex(w.weaponFormId,8)):Json()},
                {"hasGraphManager",w.hasGraphManager},{"loaded",w.loaded},{"attached",w.attached}};
            if (!w.loaded) return j;
            j["node"]=Printable(w.name);j["type"]=Printable(w.type);
            j["parent"]=w.parent.empty()?Json():Json(Printable(w.parent));
            j["world"]=ToJson(w.world);j["worldBound"]=Bound(w.boundCenter,w.boundRadius);
            j["appCulled"]=w.appCulled;j["ancestorCulled"]=w.ancestorCulled;
            return j;
        };
        return Write(writer,sink,Success({{"refId",Hex(ref->formId,8)},{"firstPerson",view(true)},{"thirdPerson",view(false)},
            {"matrixConvention",kMatrixConvention}}));
    }
    if (command=="actor.weapon_cull_state") {
        const auto ref=reference("target","player");
        if (!ref) return Write(writer,sink,error);
        if (!ref->isActor) return Write(writer,sink,Error("not_an_actor","The reference is not an actor"));
        std::string reason;
        const auto s=world_.WeaponCull(ref->formId,reason);
        if (!s) return Write(writer,sink,Error("unavailable",reason));
        static constexpr const char* kWeaponStates[]={"sheathed","want_to_draw","drawing","drawn","want_to_sheathe","sheathing"};
        Json counter=Json();
        if (s->middleHigh) counter={{"raw",s->cullCounter},{"animation",(s->cullCounter&1)!=0},{"switch",(s->cullCounter&2)!=0},
            {"counter",s->cullCounter>>2},{"culled",s->cullCounter!=0}};
        const auto nodeCull=[&](bool firstPerson,const char* name) {
            std::string missing;
            const auto n=ref->has3D?world_.Node(ref->formId,firstPerson,name,missing):std::nullopt;
            return n?Json{{"appCulled",n->appCulled},{"ancestorCulled",n->ancestorCulled}}:Json();
        };
        const auto part=[&](bool firstPerson) {
            const auto w=world_.EquippedWeapon3D(ref->formId,firstPerson);
            if (!w.biped) return Json();
            return Json{{"slot",w.slot>=0?Json(w.slot):Json()},{"loaded",w.loaded},{"hasGraphManager",w.hasGraphManager}};
        };
        Json nodes=Json::object();
        for (const bool firstPerson:{true,false}) {
            const auto* key=firstPerson?"firstPerson":"thirdPerson";
            nodes[key]={{"Weapon",nodeCull(firstPerson,"Weapon")},{"RArm_Hand",nodeCull(firstPerson,"RArm_Hand")}};
        }
        return Write(writer,sink,Success({{"refId",Hex(ref->formId,8)},
            {"weaponState",s->weaponState<6?Json(kWeaponStates[s->weaponState]):Json(s->weaponState)},{"weaponStateRaw",s->weaponState},
            {"gunState",s->gunState},{"weaponCullCounter",std::move(counter)},
            {"process",s->process?Json{{"weaponSubgraphFinishedLoading",s->subgraphLoaded},{"currentlyLoadingWeaponSet",s->loadingWeaponSet},
                {"itemEquipQueued",s->itemEquipQueued}}:Json()},
            {"nodes",std::move(nodes)},{"weaponPart",{{"firstPerson",part(true)},{"thirdPerson",part(false)}}}}));
    }
    if (command=="anim.watch") {
        const auto ref=reference("target",nullptr);
        if (!ref) return Write(writer,sink,error);
        Operation o;o.kind=Kind::Watch;o.started=now_();o.target=ref->formId;
        o.deadline=o.started+std::chrono::milliseconds(args["durationMs"].get<std::int64_t>());
        o.contains=Lower(args.value("contains",std::string()));
        o.watchId=nextWatch_++;
        std::string reason;
        if (!world_.StartAnimWatch(o.watchId,o.target,reason)) return Write(writer,sink,Error("graph_unavailable",reason));
        const auto id="world-watch-"+std::to_string(i.request_id);
        operations_[id]=std::move(o);
        return Pending(writer,sink,id);
    }
    return std::nullopt;
}

bool WorldCommands::ConditionMet(Operation& o,std::string& observed) {
    if (o.condition=="world_ready") {
        const auto s=game_.SampleReadiness();
        observed=s.sampled?(s.loadingMenuOpen?"loading menu open":s.mainMenuOpen?"main menu open":!s.parentCellAttached?"cell not attached":!s.player3DLoaded?"player 3D not loaded":"ready"):"unsampled";
        return s.sampled&&!s.loadingMenuOpen&&!s.mainMenuOpen&&s.playerPresent&&s.parentCellAttached&&s.player3DLoaded;
    }
    if (o.condition=="menu_open"||o.condition=="menu_closed") {
        const auto menus=world_.OpenMenus();
        const bool open=std::any_of(menus.begin(),menus.end(),[&](const std::string& m){return Lower(m)==Lower(o.menu);});
        observed=open?"open":"closed";
        return open==(o.condition=="menu_open");
    }
    if (o.condition=="messagebox_open"||o.condition=="messagebox_closed") {
        const bool open=world_.ReadMessageBox().open;
        observed=open?"open":"closed";
        return open==(o.condition=="messagebox_open");
    }
    const auto ref=world_.InspectRef(o.target);
    if (!ref.found) {observed="reference not loaded";return false;}
    if (o.condition=="ref_3d") {observed=ref.has3D?"3D loaded":"no 3D";return ref.has3D;}
    if (o.condition=="near_player") {
        observed="distance "+std::to_string(static_cast<int>(ref.distanceToPlayer));
        return ref.distanceToPlayer>=0&&ref.distanceToPlayer<=o.radius;
    }
    if (!ref.isActor) {observed="not an actor";return false;}
    const auto actor=world_.InspectActor(o.target,0);
    if (o.condition=="dead") {observed=actor.dead?"dead":"alive";return actor.dead;}
    observed=actor.inCombat?"in combat":"not in combat";
    return actor.inCombat==(o.condition=="in_combat");
}

void WorldCommands::Finish(Operation& o) {
    std::string ignored;
    if (o.kind==Kind::Hold) for (const auto c:o.controls) world_.SetControl(c,false,ignored);
    if (o.kind==Kind::Watch&&o.watchId) {world_.StopAnimWatch(o.watchId);o.watchId=0;}
}

std::optional<FUSIONMCP_Status> WorldCommands::Poll(const std::string& operation,FUSIONMCP_WriteFn writer,void* sink) {
    const auto it=operations_.find(operation);
    if (it==operations_.end()) return std::nullopt;
    auto& o=it->second;
    const auto now=now_();
    ++o.polls;
    const auto done=[&](const Json& envelope) {
        Finish(o);
        const auto status=Write(writer,sink,envelope);
        operations_.erase(it);
        return status;
    };
    switch (o.kind) {
    case Kind::Hold: {
        if (now<o.deadline) return FUSIONMCP_PENDING;
        Json controls=Json::array();
        for (const auto c:o.controls) controls.push_back(ControlName(c));
        return done(Success({{"controls",std::move(controls)},{"heldMs",Ms(o.started,now)},{"released",true},
            {"note","Input was delivered to the engine; whether the player moved or acted is not verified."}}));
    }
    case Kind::Wait: {
        std::string observed;
        if (ConditionMet(o,observed)) {
            if (++o.stable>=kStableSamples) return done(Success({{"condition",o.condition},{"met",true},{"observed",observed},
                {"elapsedMs",Ms(o.started,now)},{"samples",o.polls}}));
        } else o.stable=0;
        if (now>=o.deadline) return done(Error("timeout","Condition "+o.condition+" not met before timeoutMs; last observed: "+observed));
        return FUSIONMCP_PENDING;
    }
    case Kind::Spawn: {
        std::vector<RefInfo> placed;
        for (const auto& r:world_.Nearby(o.target,o.radius,"any",kSpawnScanLimit))
            if (r.base.formId==o.base&&!o.before.contains(r.formId)) placed.push_back(r);
        const bool complete=placed.size()>=o.count;
        if (complete&&++o.stable<kStableSamples) return FUSIONMCP_PENDING;
        if (!complete&&now<o.deadline) {o.stable=0;return FUSIONMCP_PENDING;}
        if (placed.empty()) return done(Error("spawn_failed","No new reference of the base form appeared near the anchor"));
        Json refs=Json::array();
        for (const auto& r:placed) refs.push_back(ToJson(r));
        return done(Success({{"base",Hex(o.base,8)},{"anchor",Hex(o.target,8)},{"requested",o.count},{"references",std::move(refs)},
            {"complete",complete},{"note","Placed by the engine's PlaceAtMe and found as new references of the base near the anchor."}}));
    }
    case Kind::Watch: {
        for (auto& e:world_.DrainAnimWatch(o.watchId)) {
            if (!o.contains.empty()&&Lower(e.tag).find(o.contains)==std::string::npos) continue;
            if (o.events.size()>=kMaxAnimEvents) {++o.dropped;continue;}
            o.events.push_back(std::move(e));
        }
        if (now<o.deadline) return FUSIONMCP_PENDING;
        Json events=Json::array();
        for (const auto& e:o.events) events.push_back({{"ms",e.ms},{"tag",Printable(e.tag)},{"payload",Printable(e.payload)}});
        return done(Success({{"refId",Hex(o.target,8)},{"durationMs",Ms(o.started,now)},{"events",std::move(events)},{"dropped",o.dropped}}));
    }
    }
    return FUSIONMCP_INTERNAL_FAILURE;
}

bool WorldCommands::Cancel(const std::string& operation) {
    const auto it=operations_.find(operation);
    if (it==operations_.end()) return false;
    Finish(it->second);
    operations_.erase(it);
    return true;
}
}
