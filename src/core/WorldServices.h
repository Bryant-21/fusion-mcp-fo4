#pragma once
// Project-owned GPL-3.0 code. Engine-free seam for world inspection and control.
// Every method runs on the Game owner unless noted; implementations copy engine
// state into these DTOs and never hand out engine pointers.
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fusionmcp {
struct Vec3 {float x=0,y=0,z=0;};
struct FormInfo {
    bool found=false;
    std::uint32_t formId=0;      // Runtime id (load-order prefixed).
    std::uint32_t localId=0;     // Id within its plugin (FormKey object id).
    std::string formType,editorId,plugin,name;
};
struct RefInfo {
    bool found=false;
    std::uint32_t formId=0;
    FormInfo base;
    std::string name,cell,worldspace;
    Vec3 position,angle;         // Angle in degrees.
    bool enabled=false,deleted=false,has3D=false,isActor=false;
    float distanceToPlayer=-1;
};
struct ItemStack {
    FormInfo item;
    std::int32_t count=0;
    bool equipped=false;
};
struct ActorInfo {
    RefInfo ref;
    bool dead=false,inCombat=false;
    float health=0,healthBase=0;
    std::uint32_t combatTarget=0;
    std::string package;
    FormInfo race;
    std::uint16_t level=0;
    std::vector<ItemStack> inventory;
    bool inventoryTruncated=false;
};
struct MessageBoxState {
    bool open=false;
    std::string text;
    std::vector<std::string> buttons;
};
struct CameraState {
    bool sampled=false;
    std::string mode;            // first, third, free, other
    float yaw=0,pitch=0;         // Player angles in degrees.
};
// Degrees as in the engine's reference angles: yaw clockwise from +Y (north),
// pitch positive looking down. Doubles so a degree value read back converts to
// the exact same stored float radians when it is set again.
struct CameraPose {Vec3 position;double yaw=0,pitch=0;};
struct CameraSnapshot {
    bool sampled=false;
    std::string mode;            // first, third, free, other
    Vec3 cameraPosition;         // Rendered camera: the camera root's world translation.
    CameraPose free;             // Free camera state (meaningful in free mode).
    CameraPose player;           // Player reference position and angles.
    float worldFov=0,firstPersonFov=0;
    bool autoVanityAllowed=true;
};
// Called on the Game owner once per rendered frame while the free camera is the
// active camera state, right before the engine reads the free camera pose.
using CameraDriver=std::function<std::optional<CameraPose>()>;
// Each typed getter is reported separately: the engine's getters do not say
// which type a graph variable actually has.
struct GraphVariable {
    std::string name;
    bool hasFloat=false,hasInt=false,hasBool=false;
    double asFloat=0;
    std::int64_t asInt=0;
    bool asBool=false;
};
struct GlobalInfo {
    bool found=false;
    FormInfo form;
    float value=0;
    bool constant=false;         // Engine refuses script writes to constant globals.
};
struct AnimEvent {
    std::int64_t ms=0;           // Since the watch started.
    std::string tag,payload;
};
// rotate is NiMatrix3::entry exactly as stored (rotate[row][col] = entry[row][col]).
// FO4 renders child = transpose(rotate) * point, the transpose of CommonLib's operator*.
struct NodeTransform {Vec3 translate;float rotate[3][3]{};float scale=1;};
struct NodeInfo {
    std::string name,type,parent;
    NodeTransform local,world;
    Vec3 boundCenter;
    float boundRadius=0;
    std::uint64_t flags=0;
    bool appCulled=false,ancestorCulled=false;
};
struct NodeEntry {
    std::uint32_t depth=0,childCount=0;
    std::string name,type;
    std::uint64_t flags=0;
    bool appCulled=false;
    Vec3 localTranslate;
};
// One clip generator on the active branch of a behaviour graph.
struct ActiveClip {
    std::string source;          // actor, actor_1p, weapon_3p, weapon_1p
    std::uint32_t graphIndex=0;
    bool activeGraph=false;
    std::string graph,node,animation,behavior;   // graph: name of the graph's root behaviour node
    float playbackSpeed=0;
    bool hasControl=false;       // hkaDefaultAnimationControl bound: localTime/controlWeight valid
    float localTime=0,controlWeight=0;
};
struct WeaponAttachment {
    bool biped=false,loaded=false,attached=false,appCulled=false,ancestorCulled=false;
    int slot=-1;                 // BIPED_OBJECT index holding the WEAP; -1 = no weapon slot
    std::uint32_t weaponFormId=0;
    bool hasGraphManager=false;  // BIPOBJECT::objectGraphManager set
    std::string name,type,parent;
    NodeTransform world;
    Vec3 boundCenter;
    float boundRadius=0;
};
struct WeaponCullState {
    std::uint32_t weaponState=0,gunState=0;   // ActorState bitfields (WEAPON_STATE, GUN_STATE)
    bool process=false,middleHigh=false;
    std::uint8_t cullCounter=0;               // MiddleHighProcessData::weaponCullCounter, raw
    bool subgraphLoaded=false,loadingWeaponSet=false,itemEquipQueued=false;
};
// Player controls, delivered as engine button events re-sent every frame while held.
enum class Control {Forward,Back,StrafeLeft,StrafeRight,Jump,Sprint,Sneak,Activate,Reload,Fire,Aim,ReadyWeapon};

class WorldServices {
public:
    virtual ~WorldServices()=default;
    // spec: "ObjectID:Plugin.esm", 8 hex digits (runtime id), "player", or an EditorID.
    virtual FormInfo ResolveForm(const std::string& spec)=0;
    virtual RefInfo InspectRef(std::uint32_t formId)=0;
    virtual ActorInfo InspectActor(std::uint32_t formId,std::size_t inventoryLimit)=0;
    // Loaded references within radius of the anchor, nearest first. kind: any, actor, or a form type.
    virtual std::vector<RefInfo> Nearby(std::uint32_t anchorId,float radius,const std::string& kind,std::size_t limit)=0;
    virtual std::vector<std::string> OpenMenus()=0;
    virtual MessageBoxState ReadMessageBox()=0;
    virtual bool AnswerMessageBox(std::uint32_t button,std::string& error)=0;
    virtual CameraState Camera()=0;
    virtual bool SetCameraMode(const std::string& mode,std::string& error)=0;
    virtual bool SetLook(float yawDeg,float pitchDeg,bool relative,std::string& error)=0;
    virtual bool FaceReference(std::uint32_t formId,std::string& error)=0;
    // Held until released; the adapter re-applies held controls every frame.
    virtual bool SetControl(Control control,bool held,std::string& error)=0;
    virtual void ReleaseAllControls()=0;
    virtual bool HudVisible()=0;
    virtual bool SetHudVisible(bool visible)=0;
    virtual std::vector<GraphVariable> GraphVariables(std::uint32_t formId,const std::vector<std::string>& names,std::string& error)=0;
    // Animation graph event capture on one reference; watch ids are caller-chosen.
    virtual bool StartAnimWatch(std::uint64_t watchId,std::uint32_t formId,std::string& error)=0;
    virtual std::vector<AnimEvent> DrainAnimWatch(std::uint64_t watchId)=0;
    virtual void StopAnimWatch(std::uint64_t watchId)=0;
    // Read-only scene graph copies. firstPerson picks Get3D(true); node empty = the 3D root.
    virtual std::optional<NodeInfo> Node(std::uint32_t formId,bool firstPerson,const std::string& node,std::string& error) {
        (void)formId;(void)firstPerson;(void)node;error="Scene graph reads are unavailable";return std::nullopt;}
    virtual std::vector<NodeEntry> NodeChildren(std::uint32_t formId,bool firstPerson,const std::string& node,std::uint32_t maxDepth,
        std::size_t limit,bool& truncated,std::string& error) {
        (void)formId;(void)firstPerson;(void)node;(void)maxDepth;(void)limit;truncated=false;error="Scene graph reads are unavailable";return {};}
    virtual std::vector<ActiveClip> ActiveClips(std::uint32_t formId,std::size_t limit,bool& truncated,std::string& error) {
        (void)formId;(void)limit;truncated=false;error="Behaviour graph reads are unavailable";return {};}
    virtual WeaponAttachment EquippedWeapon3D(std::uint32_t formId,bool firstPerson) {(void)formId;(void)firstPerson;return {};}
    virtual std::optional<WeaponCullState> WeaponCull(std::uint32_t formId,std::string& error) {
        (void)formId;error="Weapon cull state is unavailable";return std::nullopt;}
    // TESGlobal values, read and written directly (no script compiler).
    virtual GlobalInfo ReadGlobal(std::uint32_t formId) {(void)formId;return {};}
    virtual bool WriteGlobal(std::uint32_t formId,float value,std::string& error) {(void)formId;(void)value;error="Globals are unavailable";return false;}
    // The Calendar's GameHour global; 0 when unavailable.
    virtual std::uint32_t GameHourGlobal() {return 0;}
    virtual CameraSnapshot SampleCamera() {return {};}
    // Writes the free camera state; the camera shows it from the next frame.
    virtual bool SetFreeCameraPose(const CameraPose& pose,std::string& error) {(void)pose;error="Free camera control is unavailable";return false;}
    virtual bool SetPlayerAngles(double yawDeg,double pitchDeg,std::string& error) {(void)yawDeg;(void)pitchDeg;error="Player angles are unavailable";return false;}
    // An empty driver removes the current one.
    virtual bool SetCameraDriver(CameraDriver driver,std::string& error) {(void)driver;error="Per-frame camera control is unavailable";return false;}
    // The idle "vanity" third-person camera starts only while this is allowed.
    virtual void SetAutoVanityAllowed(bool allowed) {(void)allowed;}
    // The engine's own clean quit: its main loop exits after the current frame.
    virtual bool RequestQuit(std::string& error) {error="Quit is unavailable";return false;}
};
}
