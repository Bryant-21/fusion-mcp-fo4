// Original project-owned world adapter over CommonLibF4. Engine state is copied
// into DTOs before return; no engine pointer leaves this file.
#include <F4SE/F4SE.h>
#include <RE/Fallout.h>
#include "runtime/NativeWorldServices.h"
#include "GameIDs.h"
#include "core/HookChain.h"
#include <Windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>

namespace fusionmcp::runtime {
namespace {
constexpr float kDegrees=180.0f/std::numbers::pi_v<float>;

std::string Printable(std::string_view text) {
    std::string copy(text);
    for (char& c:copy) if (static_cast<unsigned char>(c)<32||static_cast<unsigned char>(c)>126) c='?';
    return copy;
}
std::string Lower(std::string text) {
    for (char& c:text) if (c>='A'&&c<='Z') c=static_cast<char>(c+('a'-'A'));
    return text;
}
std::string Upper(std::string text) {
    for (char& c:text) if (c>='a'&&c<='z') c=static_cast<char>(c-('a'-'A'));
    return text;
}
std::optional<std::uint32_t> ParseHex(std::string_view text) {
    if (text.empty()||text.size()>8) return std::nullopt;
    std::uint32_t value=0;
    for (const char c:text) {
        const int digit=c>='0'&&c<='9'?c-'0':c>='a'&&c<='f'?c-'a'+10:c>='A'&&c<='F'?c-'A'+10:-1;
        if (digit<0) return std::nullopt;
        value=value*16+static_cast<std::uint32_t>(digit);
    }
    return value;
}
const char* UserEvent(Control c) {
    switch (c) {
    case Control::Forward:return "Forward";
    case Control::Back:return "Back";
    case Control::StrafeLeft:return "StrafeLeft";
    case Control::StrafeRight:return "StrafeRight";
    case Control::Jump:return "Jump";
    case Control::Sprint:return "Sprint";
    case Control::Sneak:return "Sneak";
    case Control::Activate:return "Activate";
    case Control::Reload:return "Reload";
    case Control::Fire:return "PrimaryAttack";
    case Control::Aim:return "SecondaryAttack";
    case Control::ReadyWeapon:return "ReadyWeapon";
    }
    return "";
}
FormInfo Describe(RE::TESForm* form) {
    FormInfo info;
    if (!form) return info;
    info.found=true;
    info.formId=form->GetFormID();
    info.formType=Printable(form->GetFormTypeString()?form->GetFormTypeString():"");
    info.editorId=Printable(form->GetFormEditorID()?form->GetFormEditorID():"");
    if (const auto* file=form->GetFile(0)) {
        info.plugin=Printable(file->GetFilename());
        info.localId=form->GetLocalFormID();
    } else info.localId=info.formId;
    info.name=Printable(RE::TESFullName::GetFullName(*form));
    return info;
}
RE::TESObjectREFR* Reference(std::uint32_t id) {
    auto* form=RE::TESForm::GetFormByID(id);
    return form?form->As<RE::TESObjectREFR>():nullptr;
}
RE::NiPoint3 Position(const RE::TESObjectREFR& ref) {return ref.GetPosition();}
float Distance(const RE::NiPoint3& a,const RE::NiPoint3& b) {
    const auto dx=a.x-b.x,dy=a.y-b.y,dz=a.z-b.z;
    return std::sqrt(dx*dx+dy*dy+dz*dz);
}
RefInfo DescribeRef(RE::TESObjectREFR* ref) {
    RefInfo r;
    if (!ref) return r;
    r.found=true;
    r.formId=ref->GetFormID();
    r.base=Describe(ref->GetObjectReference());
    if (const auto* name=ref->GetDisplayFullName()) r.name=Printable(name);
    if (auto* cell=ref->GetParentCell()) {
        r.cell=Printable(cell->GetFormEditorID()?cell->GetFormEditorID():"");
        if (cell->IsExterior()&&cell->worldSpace) r.worldspace=Printable(cell->worldSpace->GetFormEditorID()?cell->worldSpace->GetFormEditorID():"");
    }
    const auto& data=ref->data;
    r.position={data.location.x,data.location.y,data.location.z};
    r.angle={data.angle.x*kDegrees,data.angle.y*kDegrees,data.angle.z*kDegrees};
    r.enabled=!ref->IsDisabled();
    r.deleted=ref->IsDeleted();
    r.has3D=ref->Get3D()!=nullptr;
    r.isActor=ref->Is(RE::ENUM_FORM_ID::kACHR);
    if (auto* player=RE::PlayerCharacter::GetSingleton()) r.distanceToPlayer=Distance(Position(*player),Position(*ref));
    return r;
}

// Synthetic player input, prepended to the engine's own event list inside its
// per-frame PlayerControls pass so the engine's handlers, blocking rules and
// movement update see it exactly as keyboard input.
struct HeldInput {
    std::string userEvent;
    std::uint32_t idCode=0;
    RE::INPUT_DEVICE device=RE::INPUT_DEVICE::kKeyboard;
    std::chrono::steady_clock::time_point since;
    bool pressSent=false,releasing=false;
};
struct InputState {
    std::mutex mutex;
    std::map<std::string,HeldInput> held;
    std::uintptr_t buttonVtable=0;
    void (*original)(RE::BSInputEventReceiver*,const RE::InputEvent*)=nullptr;
};
InputState& Input() {static auto* const state=new InputState;return *state;}

bool Chainable(std::uintptr_t current,std::uintptr_t engine) {
    HMODULE owner=nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,reinterpret_cast<LPCWSTR>(current),&owner);
    return ChainableSlot(current,engine,owner,GetModuleHandleW(nullptr));
}

void HookedPerformInputProcessing(RE::BSInputEventReceiver* self,const RE::InputEvent* head) {
    auto& input=Input();
    constexpr std::size_t kMaxEvents=16;
    std::array<RE::ButtonEvent,kMaxEvents> events;
    std::size_t used=0;
    {
        std::scoped_lock lock(input.mutex);
        const auto now=std::chrono::steady_clock::now();
        for (auto it=input.held.begin();it!=input.held.end()&&used<kMaxEvents;) {
            auto& h=it->second;
            auto& e=events[used++];
            *reinterpret_cast<std::uintptr_t*>(&e)=input.buttonVtable;
            e.device=h.device;e.eventType=RE::INPUT_EVENT_TYPE::kButton;
            e.strUserEvent=RE::BSFixedString(h.userEvent);e.idCode=static_cast<std::int32_t>(h.idCode);
            const float secs=h.pressSent?std::chrono::duration<float>(now-h.since).count():0.0f;
            e.heldDownSecs=secs;
            if (h.releasing&&h.pressSent) {
                e.value=0.0f;
                it=input.held.erase(it);
                continue;
            }
            e.value=1.0f;
            if (!h.pressSent) {h.pressSent=true;h.since=now;}
            ++it;
        }
    }
    for (std::size_t n=0;n+1<used;++n) events[n].next=&events[n+1];
    if (used) events[used-1].next=const_cast<RE::InputEvent*>(head);
    input.original(self,used?&events[0]:head);
}
}

bool NativeWorldServices::InstallInputHook(std::string& error) {
    auto& input=Input();
    if (input.original) return true;
    if (slot_<0) {error="Input is not verified on this runtime";return false;}
    auto* controls=RE::PlayerControls::GetSingleton();
    if (!controls) {error="PlayerControls is not initialised yet";return false;}
    auto** table=*reinterpret_cast<std::uintptr_t***>(static_cast<RE::BSInputEventReceiver*>(controls));
    const auto current=reinterpret_cast<std::uintptr_t>(table[0]);
    if (!Chainable(current,ids::Input::PerformInputProcessing.address())) {
        error="PlayerControls input slot is not the expected engine function or another plugin's hook (the runtime differs)";
        return false;
    }
    input.buttonVtable=ids::Input::ButtonEventVtable.address();
    input.original=reinterpret_cast<decltype(input.original)>(current);
    DWORD previous=0;
    if (!VirtualProtect(&table[0],sizeof(void*),PAGE_EXECUTE_READWRITE,&previous)) {input.original=nullptr;error="vtable is not writable";return false;}
    table[0]=reinterpret_cast<std::uintptr_t*>(&HookedPerformInputProcessing);
    VirtualProtect(&table[0],sizeof(void*),previous,&previous);
    REX::INFO("FusionMCP input hook installed on PlayerControls::PerformInputProcessing{}",current==ids::Input::PerformInputProcessing.address()?"":" (chained through another plugin's hook)");
    return true;
}

bool NativeWorldServices::SetControl(Control control,bool held,std::string& error) {
    const std::string userEvent=UserEvent(control);
    auto& input=Input();
    if (!held) {
        std::scoped_lock lock(input.mutex);
        if (const auto it=input.held.find(userEvent);it!=input.held.end()) it->second.releasing=true;
        return true;
    }
    if (!InstallInputHook(error)) return false;
    const auto* map=RE::ControlMap::GetSingleton();
    if (!map) {error="ControlMap unavailable";return false;}
    HeldInput h;h.userEvent=userEvent;
    h.idCode=map->GetMappedKey(userEvent,RE::INPUT_DEVICE::kKeyboard);
    if (h.idCode==RE::ControlMap::kInvalid) {
        h.idCode=map->GetMappedKey(userEvent,RE::INPUT_DEVICE::kMouse);
        h.device=RE::INPUT_DEVICE::kMouse;
    }
    if (h.idCode==RE::ControlMap::kInvalid) {error="No key or mouse button is mapped to user event "+userEvent;return false;}
    std::scoped_lock lock(input.mutex);
    input.held[userEvent]=std::move(h);
    return true;
}
void NativeWorldServices::ReleaseAllControls() {
    auto& input=Input();
    std::scoped_lock lock(input.mutex);
    for (auto& [name,h]:input.held) h.releasing=true;
}

FormInfo NativeWorldServices::ResolveForm(const std::string& spec) {
    RE::TESForm* form=nullptr;
    if (Lower(spec)=="player") form=RE::PlayerCharacter::GetSingleton();
    else if (const auto colon=spec.find(':');colon!=std::string::npos) {
        const auto raw=ParseHex(std::string_view(spec).substr(0,colon));
        auto* handler=RE::TESDataHandler::GetSingleton();
        if (raw&&handler) form=handler->LookupForm(*raw&0xFFFFFF,spec.substr(colon+1));
    } else if (spec.size()==8&&ParseHex(spec)) form=RE::TESForm::GetFormByID(*ParseHex(spec));
    else form=RE::TESForm::GetFormByEditorID(RE::BSFixedString(spec));
    return Describe(form);
}
RefInfo NativeWorldServices::InspectRef(std::uint32_t formId) {return DescribeRef(Reference(formId));}

ActorInfo NativeWorldServices::InspectActor(std::uint32_t formId,std::size_t inventoryLimit) {
    ActorInfo info;
    auto* ref=Reference(formId);
    auto* actor=ref?ref->As<RE::Actor>():nullptr;
    if (!actor) return info;
    info.ref=DescribeRef(actor);
    info.dead=actor->IsDead(false);
    info.inCombat=actor->IsInCombat();
    if (auto* values=RE::ActorValue::GetSingleton();values&&values->health) {
        info.health=actor->GetActorValue(*values->health);
        info.healthBase=actor->GetPermanentActorValue(*values->health);
    }
    if (auto target=actor->currentCombatTarget.get()) info.combatTarget=target->GetFormID();
    if (actor->currentProcess&&actor->currentProcess->currentPackage.package)
        info.package=Printable(actor->currentProcess->currentPackage.package->GetFormEditorID()?actor->currentProcess->currentPackage.package->GetFormEditorID():"");
    info.race=Describe(actor->race);
    info.level=static_cast<std::uint16_t>(std::max<std::int16_t>(0,actor->GetLevel()));
    if (auto* inventory=actor->inventoryList) {
        RE::BSAutoReadLock lock{inventory->rwLock};
        for (auto& item:inventory->data) {
            if (!item.object) continue;
            if (info.inventory.size()>=inventoryLimit) {info.inventoryTruncated=true;break;}
            ItemStack stack;stack.item=Describe(item.object);
            for (auto* s=item.stackData.get();s;s=s->nextStack.get()) {stack.count+=static_cast<std::int32_t>(s->count);stack.equipped|=s->IsEquipped();}
            info.inventory.push_back(std::move(stack));
        }
    }
    return info;
}

std::vector<RefInfo> NativeWorldServices::Nearby(std::uint32_t anchorId,float radius,const std::string& kind,std::size_t limit) {
    std::vector<std::pair<float,RE::TESObjectREFR*>> found;
    auto* anchor=Reference(anchorId);
    auto* tes=RE::TES::GetSingleton();
    if (!anchor||!tes) return {};
    const auto origin=Position(*anchor);
    const auto wanted=Upper(kind);
    tes->ForEachReferenceInRange(anchor,radius,[&](RE::TESObjectREFR* ref) {
        if (ref&&ref!=anchor&&!ref->IsDeleted()) {
            const auto* base=ref->GetObjectReference();
            const bool match=wanted=="ANY"||(wanted=="ACTOR"&&ref->Is(RE::ENUM_FORM_ID::kACHR))||
                (base&&base->GetFormTypeString()&&wanted==base->GetFormTypeString());
            if (match) found.emplace_back(Distance(origin,Position(*ref)),ref);
        }
        return RE::BSContainer::ForEachResult::kContinue;
    });
    std::sort(found.begin(),found.end(),[](const auto& a,const auto& b){return a.first<b.first;});
    std::vector<RefInfo> refs;
    for (const auto& [distance,ref]:found) {
        if (refs.size()>=limit) break;
        refs.push_back(DescribeRef(ref));
    }
    return refs;
}

std::vector<std::string> NativeWorldServices::OpenMenus() {
    std::vector<std::string> menus;
    auto* ui=RE::UI::GetSingleton();
    if (!ui) return menus;
    for (const auto& menu:ui->menuStack) if (menu) menus.push_back(Printable(menu->menuName.c_str()));
    return menus;
}

CameraState NativeWorldServices::Camera() {
    CameraState state;
    auto* camera=RE::PlayerCamera::GetSingleton();
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (!camera||!player) return state;
    state.sampled=true;
    state.mode=camera->QCameraEquals(RE::CameraState::kFree)?"free":camera->QCameraEquals(RE::CameraState::kFirstPerson)?"first":
        camera->QCameraEquals(RE::CameraState::k3rdPerson)?"third":"other";
    state.yaw=player->data.angle.z*kDegrees;
    state.pitch=player->data.angle.x*kDegrees;
    return state;
}
bool NativeWorldServices::SetCameraMode(const std::string& mode,std::string& error) {
    auto* camera=RE::PlayerCamera::GetSingleton();
    if (!camera) {error="PlayerCamera unavailable";return false;}
    const bool free=camera->QCameraEquals(RE::CameraState::kFree);
    if (mode=="free") {
        if (!free) camera->ToggleFreeCameraMode(false);
        return true;
    }
    if (free) camera->ToggleFreeCameraMode(false);
    const bool first=camera->QCameraEquals(RE::CameraState::kFirstPerson);
    if ((mode=="first")==first&&!free) return true;
    // TogglePOV is consumed by the camera's own input receiver, not PlayerControls,
    // so call the functions its first/third person states use.
    if (slot_<0) {error="Camera mode is not verified on this runtime";return false;}
    const auto force=mode=="first"?ids::Camera::Force1stPerson.address():ids::Camera::Force3rdPerson.address();
    reinterpret_cast<void(*)(RE::PlayerCamera*)>(force)(camera);
    return true;
}
bool NativeWorldServices::SetLook(float yawDeg,float pitchDeg,bool relative,std::string& error) {
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (!player) {error="Player unavailable";return false;}
    const auto current=player->data.angle;
    float yaw=(relative?current.z*kDegrees+yawDeg:yawDeg);
    float pitch=std::clamp(relative?current.x*kDegrees+pitchDeg:pitchDeg,-89.0f,89.0f);
    yaw=std::fmod(yaw,360.0f);
    if (yaw<0) yaw+=360.0f;
    player->SetAngleOnReference(RE::NiPoint3{pitch/kDegrees,current.y,yaw/kDegrees});
    return true;
}
bool NativeWorldServices::FaceReference(std::uint32_t formId,std::string& error) {
    auto* player=RE::PlayerCharacter::GetSingleton();
    auto* target=Reference(formId);
    if (!player||!target) {error="Reference not loaded";return false;}
    const auto from=Position(*player),to=Position(*target);
    const float dx=to.x-from.x,dy=to.y-from.y;
    // Aim from eye height at the target's mid-height.
    const float dz=(to.z+64.0f)-(from.z+120.0f);
    const float yaw=std::atan2(dx,dy)*kDegrees;
    const float pitch=-std::atan2(dz,std::sqrt(dx*dx+dy*dy))*kDegrees;
    return SetLook(yaw,pitch,false,error);
}

namespace {
RE::TESGlobal* Global(std::uint32_t formId) {
    auto* form=RE::TESForm::GetFormByID(formId);
    return form&&form->GetFormType()==RE::ENUM_FORM_ID::kGLOB?static_cast<RE::TESGlobal*>(form):nullptr;
}
// TESGlobal record flag "Constant".
constexpr std::uint32_t kConstantGlobal=0x40;
}
GlobalInfo NativeWorldServices::ReadGlobal(std::uint32_t formId) {
    GlobalInfo info;
    auto* global=Global(formId);
    if (!global) return info;
    info.found=true;info.form=Describe(global);info.value=global->value;
    info.constant=(global->formFlags&kConstantGlobal)!=0;
    if (info.form.editorId.empty()&&global->formEditorID.c_str()) info.form.editorId=Printable(global->formEditorID.c_str());
    return info;
}
bool NativeWorldServices::WriteGlobal(std::uint32_t formId,float value,std::string& error) {
    auto* global=Global(formId);
    if (!global) {error="Not a global variable";return false;}
    global->value=value;
    return true;
}
std::uint32_t NativeWorldServices::GameHourGlobal() {
    const auto* calendar=RE::Calendar::GetSingleton();
    return calendar&&calendar->gameHour?calendar->gameHour->GetFormID():0;
}
std::vector<GraphVariable> NativeWorldServices::GraphVariables(std::uint32_t formId,const std::vector<std::string>& names,std::string& error) {
    std::vector<GraphVariable> values;
    auto* ref=Reference(formId);
    if (!ref) {error="Reference not loaded";return values;}
    const auto* holder=static_cast<const RE::IAnimationGraphManagerHolder*>(ref);
    for (const auto& name:names) {
        GraphVariable v;v.name=name;
        const RE::BSFixedString variable(name);
        float f=0;std::int32_t i=0;bool b=false;
        if (holder->GetGraphVariableImplFloat(variable,f)) {v.hasFloat=true;v.asFloat=f;}
        if (holder->GetGraphVariableImplInt(variable,i)) {v.hasInt=true;v.asInt=i;}
        if (holder->GetGraphVariableImplBool(variable,b)) {v.hasBool=true;v.asBool=b;}
        values.push_back(std::move(v));
    }
    return values;
}

namespace {
std::string NiName(const RE::NiAVObject& node) {return Printable(node.name.c_str()?node.name.c_str():"");}
std::string NiType(const RE::NiAVObject& node) {
    const auto* rtti=node.GetRTTI();
    return rtti&&rtti->GetName()?Printable(rtti->GetName()):"";
}
NodeTransform Copy(const RE::NiTransform& t) {
    NodeTransform out;
    out.translate={t.translate.x,t.translate.y,t.translate.z};
    for (int r=0;r<3;++r) for (int c=0;c<3;++c) out.rotate[r][c]=t.rotate.entry[r][c];
    out.scale=t.scale;
    return out;
}
bool AncestorCulled(const RE::NiAVObject& node) {
    for (const auto* p=node.parent;p;p=p->parent) if (p->GetAppCulled()) return true;
    return false;
}
RE::NiAVObject* Root3D(std::uint32_t formId,bool firstPerson,std::string& error) {
    auto* ref=Reference(formId);
    if (!ref) {error="Reference not loaded";return nullptr;}
    auto* root=ref->Get3D(firstPerson);
    if (!root) error=firstPerson?"No first-person 3D (only the player has one)":"No 3D loaded";
    return root;
}
RE::NiAVObject* FindNode(RE::NiAVObject* root,const std::string& name,std::string& error) {
    if (!root||name.empty()) return root;
    auto* node=root->GetObjectByName(RE::BSFixedString(name));
    if (!node) error="No node named "+Printable(name)+" under the 3D root";
    return node;
}
}
std::optional<NodeInfo> NativeWorldServices::Node(std::uint32_t formId,bool firstPerson,const std::string& name,std::string& error) {
    auto* node=FindNode(Root3D(formId,firstPerson,error),name,error);
    if (!node) return std::nullopt;
    NodeInfo info;
    info.name=NiName(*node);info.type=NiType(*node);
    if (node->parent) info.parent=NiName(*node->parent);
    info.local=Copy(node->local);info.world=Copy(node->world);
    info.boundCenter={node->worldBound.center.x,node->worldBound.center.y,node->worldBound.center.z};
    info.boundRadius=node->worldBound.fRadius;
    info.flags=node->GetFlags();info.appCulled=node->GetAppCulled();info.ancestorCulled=AncestorCulled(*node);
    return info;
}
std::vector<NodeEntry> NativeWorldServices::NodeChildren(std::uint32_t formId,bool firstPerson,const std::string& name,std::uint32_t maxDepth,
    std::size_t limit,bool& truncated,std::string& error) {
    std::vector<NodeEntry> out;
    truncated=false;
    auto* start=FindNode(Root3D(formId,firstPerson,error),name,error);
    if (!start) return out;
    // Explicit stack, depth-first in child-array order. Bounded three ways: output entries, pending
    // stack entries and a wall-clock budget, so a large 1P tree cannot stall the owner frame.
    constexpr std::size_t kMaxPending=4096;
    constexpr auto kBudget=std::chrono::milliseconds(20);
    const auto started=std::chrono::steady_clock::now();
    out.reserve(limit);
    std::vector<std::pair<RE::NiAVObject*,std::uint32_t>> stack;
    stack.reserve(256);
    stack.emplace_back(start,0);
    std::size_t visited=0;
    while (!stack.empty()) {
        if (out.size()>=limit||(++visited%32==0&&std::chrono::steady_clock::now()-started>kBudget)) {truncated=true;break;}
        const auto [node,depth]=stack.back();
        stack.pop_back();
        NodeEntry e;e.depth=depth;e.name=NiName(*node);e.type=NiType(*node);
        e.flags=node->GetFlags();e.appCulled=node->GetAppCulled();
        e.localTranslate={node->local.translate.x,node->local.translate.y,node->local.translate.z};
        auto* asNode=node->IsNode();
        const std::uint16_t capacity=asNode?asNode->children.capacity():0;
        for (std::uint16_t n=0;n<capacity;++n) if (asNode->children[n]) ++e.childCount;
        out.push_back(std::move(e));
        if (depth>=maxDepth||!out.back().childCount) continue;
        if (stack.size()+capacity>kMaxPending) {truncated=true;continue;}
        for (std::uint16_t n=capacity;n-->0;) if (auto* child=asNode->children[n].get()) stack.emplace_back(child,depth+1);
    }
    if (const auto ms=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-started).count();ms>5)
        REX::INFO("FusionMCP node.children walked {} entries in {:.1f} ms{}",out.size(),ms,truncated?" (truncated)":"");
    return out;
}

namespace {
// Active clip generators through the engine's own BSAnimGraphVisit::BShkbVisitor traversal
// (vtable slots: 0 dtor, 1 traverse, 2 visit), as FO4's ClipTimeVisitor does. The traversal
// reads only +0x08 (hkbCharacter) and +0x10 (child flags) off the visitor.
constexpr std::size_t kMaxVisitedClips=128;
constexpr std::int32_t kMaxGraphDepth=64;
const char* HkString(const void* object,std::size_t offset) {
    const auto raw=*reinterpret_cast<const std::uintptr_t*>(static_cast<const std::byte*>(object)+offset);
    return reinterpret_cast<const char*>(raw&~std::uintptr_t{1});
}
template <class T> T Field(const void* object,std::size_t offset) {return *reinterpret_cast<const T*>(static_cast<const std::byte*>(object)+offset);}
struct ClipVisitor {
    const void* vtable;
    void* character;
    std::int32_t childFlags;
    std::int32_t depth;
    const char* behaviorAtDepth[kMaxGraphDepth];
    struct Entry {const void* node;const char* behavior;};
    Entry clips[kMaxVisitedClips];
    std::size_t count,overflow;
};
static_assert(offsetof(ClipVisitor,character)==0x08);
static_assert(offsetof(ClipVisitor,childFlags)==0x10);
using TraverseFn=std::uint32_t (*)(void*,void*);
TraverseFn EngineTraverse() {
    static const TraverseFn traverse=[] {
        REL::Relocation<std::uintptr_t> vtable{RE::VTABLE::BSAnimGraphVisit__BShkbVisitor[0]};
        return *reinterpret_cast<TraverseFn*>(vtable.address()+sizeof(void*));
    }();
    return traverse;
}
// Runs inside the engine's traversal: no allocation, no throw.
std::uint32_t DescendClips(void* self,void* node) {
    auto& v=*static_cast<ClipVisitor*>(self);
    if (v.depth+1>=kMaxGraphDepth) return 0;
    ++v.depth;
    v.behaviorAtDepth[v.depth]=nullptr;
    const auto control=EngineTraverse()(self,node);
    --v.depth;
    return control;
}
std::uint32_t VisitClip(void* self,void* node) {
    auto& v=*static_cast<ClipVisitor*>(self);
    const auto type=Field<std::uint8_t>(node,ids::Behavior::NodeType);
    if (type==ids::Behavior::NodeTypeBehaviorReference) v.behaviorAtDepth[v.depth]=HkString(node,ids::Behavior::BehaviorReferenceName);
    else if (type==ids::Behavior::NodeTypeClipGenerator) {
        if (v.count>=kMaxVisitedClips) {++v.overflow;return 0;}
        const char* behavior=nullptr;
        for (auto d=v.depth;d>=0&&!behavior;--d) behavior=v.behaviorAtDepth[d];
        v.clips[v.count++]={node,behavior};
    }
    return 0;
}
void* KeepVisitor(void* self,std::uint32_t) {return self;}
using AnyFn=void (*)();
const AnyFn kClipVisitorVtable[3]={reinterpret_cast<AnyFn>(&KeepVisitor),reinterpret_cast<AnyFn>(&DescendClips),reinterpret_cast<AnyFn>(&VisitClip)};

void CollectClips(RE::BSAnimationGraphManager& manager,const std::string& source,std::size_t limit,std::vector<ActiveClip>& out,bool& truncated) {
    // The lock BSAnimationGraphManager::VisitActiveGraph takes: the animation job may retire nodes under a reader.
    const RE::BSAutoLock lock{manager.updateLock};
    for (std::uint32_t index=0;index<manager.graph.size();++index) {
        auto* graph=manager.graph[index].get();
        if (!graph) continue;
        const auto base=reinterpret_cast<std::uintptr_t>(graph);
        auto* root=*reinterpret_cast<void**>(base+ids::Behavior::GraphRootBehavior);
        if (!root) continue;
        auto visitor=std::make_unique<ClipVisitor>();
        visitor->vtable=kClipVisitorVtable;
        visitor->character=reinterpret_cast<void*>(base+ids::Behavior::GraphCharacter);
        visitor->childFlags=ids::Behavior::ActiveChildrenOnly;
        visitor->depth=-1;
        DescendClips(visitor.get(),root);
        if (visitor->overflow) truncated=true;
        const auto* graphName=HkString(root,ids::Behavior::NodeName);
        for (std::size_t n=0;n<visitor->count;++n) {
            if (out.size()>=limit) {truncated=true;return;}
            const auto* node=visitor->clips[n].node;
            ActiveClip c;c.source=source;c.graphIndex=index;c.activeGraph=index==manager.activeGraph;
            c.graph=Printable(graphName?graphName:"");
            const auto* clipName=HkString(node,ids::Behavior::NodeName);
            const auto* animation=HkString(node,ids::Behavior::ClipAnimationName);
            c.node=Printable(clipName?clipName:"");c.animation=Printable(animation?animation:"");
            if (visitor->clips[n].behavior) c.behavior=Printable(visitor->clips[n].behavior);
            c.playbackSpeed=Field<float>(node,ids::Behavior::ClipPlaybackSpeed);
            if (const auto* control=Field<const void*>(node,ids::Behavior::ClipAnimationControl)) {
                c.hasControl=true;
                c.localTime=Field<float>(control,ids::Behavior::ControlLocalTime);
                c.controlWeight=Field<float>(control,ids::Behavior::ControlWeight);
            }
            out.push_back(std::move(c));
        }
    }
}
// BipedAnim::AttachBipedWeapon stores the weapon in the slot BipedAnim::GetBipedObject picks per
// weapon type (gun, melee, grenade...), not kWeaponHand: take the slot whose parent form is a
// WEAP, preferring one with a loaded part clone.
struct WeaponSlot {RE::BIPOBJECT* object=nullptr;int slot=-1;};
WeaponSlot WeaponBiped(RE::TESObjectREFR& ref,bool firstPerson,bool& hasBiped) {
    auto* biped=ref.GetBiped(firstPerson).get();
    hasBiped=biped!=nullptr;
    WeaponSlot found;
    if (!biped) return found;
    for (int n=0;n<static_cast<int>(std::to_underlying(RE::BIPED_OBJECT::kTotal));++n) {
        auto& object=biped->object[n];
        if (!object.parent.object||!object.parent.object->Is(RE::ENUM_FORM_ID::kWEAP)) continue;
        if (!found.object||(!found.object->partClone&&object.partClone)) found={&object,n};
        if (object.partClone) break;
    }
    return found;
}
}
std::vector<ActiveClip> NativeWorldServices::ActiveClips(std::uint32_t formId,std::size_t limit,bool& truncated,std::string& error) {
    std::vector<ActiveClip> clips;
    truncated=false;
    auto* ref=Reference(formId);
    if (!ref) {error="Reference not loaded";return clips;}
    RE::BSTSmartPointer<RE::BSAnimationGraphManager> manager;
    const auto* holder=static_cast<const RE::IAnimationGraphManagerHolder*>(ref);
    const bool hasGraph=holder->GetAnimationGraphManagerImpl(manager)&&manager;
    if (hasGraph) CollectClips(*manager.get(),"actor",limit,clips,truncated);
    bool weaponGraph=false;
    if (ref->Is(RE::ENUM_FORM_ID::kACHR)) {
        const bool player=ref==RE::PlayerCharacter::GetSingleton();
        for (const bool firstPerson:{false,true}) {
            if (firstPerson&&!player) continue;
            bool hasBiped=false;
            auto* weapon=WeaponBiped(*ref,firstPerson,hasBiped).object;
            auto* weaponHolder=weapon?weapon->objectGraphManager.get():nullptr;
            if (!weaponHolder||!weaponHolder->animationGraphManager) continue;
            weaponGraph=true;
            CollectClips(*weaponHolder->animationGraphManager.get(),firstPerson?"weapon_1p":"weapon_3p",limit,clips,truncated);
        }
    }
    if (!hasGraph&&!weaponGraph) error="The reference has no animation graph";
    return clips;
}
WeaponAttachment NativeWorldServices::EquippedWeapon3D(std::uint32_t formId,bool firstPerson) {
    WeaponAttachment w;
    auto* ref=Reference(formId);
    if (!ref||(firstPerson&&ref!=RE::PlayerCharacter::GetSingleton())) return w;
    const auto found=WeaponBiped(*ref,firstPerson,w.biped);
    auto* weapon=found.object;
    if (!weapon) return w;
    w.slot=found.slot;
    w.weaponFormId=weapon->parent.object->GetFormID();
    w.hasGraphManager=static_cast<bool>(weapon->objectGraphManager);
    auto* part=weapon->partClone.get();
    if (!part) return w;
    w.loaded=true;
    w.name=NiName(*part);w.type=NiType(*part);
    if (part->parent) w.parent=NiName(*part->parent);
    const auto* root=ref->Get3D(firstPerson);
    for (const auto* p=part->parent;p&&root;p=p->parent) if (p==root) {w.attached=true;break;}
    w.world=Copy(part->world);
    w.boundCenter={part->worldBound.center.x,part->worldBound.center.y,part->worldBound.center.z};
    w.boundRadius=part->worldBound.fRadius;
    w.appCulled=part->GetAppCulled();w.ancestorCulled=AncestorCulled(*part);
    return w;
}
std::optional<WeaponCullState> NativeWorldServices::WeaponCull(std::uint32_t formId,std::string& error) {
    auto* ref=Reference(formId);
    auto* actor=ref?ref->As<RE::Actor>():nullptr;
    if (!actor) {error="The reference is not a loaded actor";return std::nullopt;}
    WeaponCullState s;
    s.weaponState=static_cast<std::uint32_t>(actor->weaponState);
    s.gunState=static_cast<std::uint32_t>(actor->gunState);
    auto* process=actor->currentProcess;
    if (!process) return s;
    s.process=true;
    if (process->middleHigh) {
        s.middleHigh=true;
        // MiddleHighProcessData+0x498, the byte AIProcess::QCodeWeaponCulled returns on 1.10.163 and 1.11.240.
        s.cullCounter=static_cast<std::uint8_t>(process->middleHigh->weaponCullCounter);
    }
    s.subgraphLoaded=process->IsWeaponSubgraphFinishedLoading(*actor);
    s.loadingWeaponSet=REL::Relocation<bool(*)(RE::AIProcess*)>{ids::AIProcess::CurrentlyLoadingWeaponSet}(process);
    s.itemEquipQueued=REL::Relocation<bool(*)(RE::AIProcess*)>{ids::AIProcess::IsItemEquipQueued}(process);
    return s;
}

namespace {
// MessageBoxData offsets live in GameIDs.h. BSString is {char* data; u16 length; u16 capacity}.
struct RawString {const char* data;std::uint16_t length;std::uint16_t capacity;std::uint32_t pad;};
static_assert(sizeof(RawString)==0x10);
std::string Read(const RawString& s) {
    std::string text;
    if (!s.data) return text;
    for (const char c:std::string_view(s.data,s.length)) {
        if (c=='\r') continue;
        const auto u=static_cast<unsigned char>(c);
        text.push_back(c=='\n'||(u>=32&&u<=126)?c:'?');
    }
    return text;
}
std::byte* CurrentMessage() {
    auto* ui=RE::UI::GetSingleton();
    if (!ui||!ui->GetMenuOpen(RE::BSFixedString("MessageBoxMenu"))) return nullptr;
    const auto menu=ui->GetMenu(RE::BSFixedString("MessageBoxMenu"));
    if (!menu) return nullptr;
    return *reinterpret_cast<std::byte**>(reinterpret_cast<std::byte*>(menu.get())+ids::MessageBoxMenu::CurrentMessage.value());
}

struct AnimWatch {std::uint32_t formId=0;std::chrono::steady_clock::time_point started;std::vector<AnimEvent> events;};
struct AnimState {
    std::mutex mutex;
    std::map<std::uint64_t,AnimWatch> watches;
    using ProcessEvent=std::uint32_t (*)(void*,const RE::BSAnimationGraphEvent*,void*);
    // Per table: another plugin may hook only one of them.
    ProcessEvent actorOriginal=nullptr,playerOriginal=nullptr;
    std::uintptr_t playerTable=0;
    bool installed=false;
};
AnimState& Anim() {static auto* const state=new AnimState;return *state;}
constexpr std::size_t kMaxBufferedAnimEvents=4096;
// Graph events can arrive on animation job threads: copy and return quickly.
std::uint32_t HookedAnimEvent(void* sink,const RE::BSAnimationGraphEvent* event,void* source) {
    auto& anim=Anim();
    const auto original=*static_cast<std::uintptr_t*>(sink)==anim.playerTable?anim.playerOriginal:anim.actorOriginal;
    const auto result=original(sink,event,source);
    if (!event) return result;
    const auto* ref=reinterpret_cast<const RE::TESObjectREFR*>(static_cast<std::byte*>(sink)-ids::AnimEvents::SinkInReference);
    std::scoped_lock lock(anim.mutex);
    if (anim.watches.empty()) return result;
    const auto id=ref->GetFormID();
    for (auto& [watchId,watch]:anim.watches) {
        if (watch.formId!=id||watch.events.size()>=kMaxBufferedAnimEvents) continue;
        watch.events.push_back({std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now()-watch.started).count(),
            event->tag.c_str()?event->tag.c_str():"",event->payload.c_str()?event->payload.c_str():""});
    }
    return result;
}
bool PatchSlot(std::uintptr_t* table,std::size_t index,std::uintptr_t replacement) {
    DWORD previous=0;
    if (!VirtualProtect(&table[index],sizeof(void*),PAGE_EXECUTE_READWRITE,&previous)) return false;
    table[index]=replacement;
    VirtualProtect(&table[index],sizeof(void*),previous,&previous);
    return true;
}
}

MessageBoxState NativeWorldServices::ReadMessageBox() {
    MessageBoxState state;
    auto* message=slot_>=0?CurrentMessage():nullptr;
    if (!message) return state;
    state.open=true;
    const auto header=Read(*reinterpret_cast<const RawString*>(message+ids::MessageBoxMenu::DataHeader));
    const auto body=Read(*reinterpret_cast<const RawString*>(message+ids::MessageBoxMenu::DataBody));
    state.text=header.empty()?body:header+"\n"+body;
    const auto* buttons=*reinterpret_cast<const RawString* const*>(message+ids::MessageBoxMenu::DataButtons);
    const auto count=*reinterpret_cast<const std::uint32_t*>(message+ids::MessageBoxMenu::DataButtonCount);
    for (std::uint32_t n=0;buttons&&n<count&&n<32;++n) state.buttons.push_back(Read(buttons[n]));
    return state;
}
// Exactly what a click does: MessageBoxMenu::OnButtonPress(menu, index), queued as a
// UI task where Scaleform callbacks run. It shows the next queued message, then
// invokes the message's callback.
bool NativeWorldServices::AnswerMessageBox(std::uint32_t button,std::string& error) {
    auto* message=slot_>=0?CurrentMessage():nullptr;
    if (!message) {error="No engine message box is visible";return false;}
    if (button>=*reinterpret_cast<const std::uint32_t*>(message+ids::MessageBoxMenu::DataButtonCount)) {error="Button index out of range";return false;}
    const auto* tasks=F4SE::GetTaskInterface();
    if (!tasks) {error="F4SE task interface unavailable";return false;}
    const auto press=ids::MessageBoxMenu::OnButtonPress.address();
    tasks->AddUITask([press,message,button] {
        auto* ui=RE::UI::GetSingleton();
        if (!ui||CurrentMessage()!=message) return;  // Answered or replaced meanwhile.
        const auto menu=ui->GetMenu(RE::BSFixedString("MessageBoxMenu"));
        if (menu) reinterpret_cast<void(*)(RE::IMenu*,std::uint8_t)>(press)(menu.get(),static_cast<std::uint8_t>(button));
    });
    return true;
}

bool NativeWorldServices::HudVisible() {
    auto* ui=RE::UI::GetSingleton();
    return !ui||slot_<0||*reinterpret_cast<const bool*>(reinterpret_cast<const std::byte*>(ui)+ids::UI::MenuSystemVisible.value());
}
// The engine's own ToggleMenus ("tm") flips the flag and the 3D interface renderers.
bool NativeWorldServices::SetHudVisible(bool visible) {
    if (HudVisible()==visible) return true;
    RE::Console::ExecuteCommand("ToggleMenus");
    return true;
}

bool NativeWorldServices::StartAnimWatch(std::uint64_t watchId,std::uint32_t formId,std::string& error) {
    auto* ref=Reference(formId);
    if (!ref) {error="Reference not loaded";return false;}
    if (!ref->Is(RE::ENUM_FORM_ID::kACHR)) {error="Animation event capture supports actors only";return false;}
    auto& anim=Anim();
    std::scoped_lock lock(anim.mutex);
    if (!anim.installed) {
        // Actor and PlayerCharacter sink vtables share Actor::ProcessEvent in slot 1.
        const auto expected=ids::AnimEvents::ActorProcessEvent.address();
        auto* actorTable=reinterpret_cast<std::uintptr_t*>(ids::AnimEvents::ActorSinkVtable.address());
        auto* playerTable=reinterpret_cast<std::uintptr_t*>(ids::AnimEvents::PlayerSinkVtable.address());
        if (!Chainable(actorTable[1],expected)||!Chainable(playerTable[1],expected)) {
            error="Animation event sink slot is not the expected engine function or another plugin's hook (the runtime differs)";
            return false;
        }
        anim.actorOriginal=reinterpret_cast<AnimState::ProcessEvent>(actorTable[1]);
        anim.playerOriginal=reinterpret_cast<AnimState::ProcessEvent>(playerTable[1]);
        anim.playerTable=reinterpret_cast<std::uintptr_t>(playerTable);
        if (!PatchSlot(actorTable,1,reinterpret_cast<std::uintptr_t>(&HookedAnimEvent))||
            !PatchSlot(playerTable,1,reinterpret_cast<std::uintptr_t>(&HookedAnimEvent))) {error="vtable is not writable";return false;}
        anim.installed=true;
        REX::INFO("FusionMCP animation graph event hook installed");
    }
    anim.watches[watchId]={formId,std::chrono::steady_clock::now(),{}};
    return true;
}
std::vector<AnimEvent> NativeWorldServices::DrainAnimWatch(std::uint64_t watchId) {
    auto& anim=Anim();
    std::scoped_lock lock(anim.mutex);
    const auto it=anim.watches.find(watchId);
    return it==anim.watches.end()?std::vector<AnimEvent>{}:std::exchange(it->second.events,{});
}
void NativeWorldServices::StopAnimWatch(std::uint64_t watchId) {
    auto& anim=Anim();
    std::scoped_lock lock(anim.mutex);
    anim.watches.erase(watchId);
}

namespace {
constexpr double kTwoPi=2.0*std::numbers::pi;
constexpr double kDegreesD=180.0/std::numbers::pi;
// Radians kept in [0,2pi), the range FreeCameraState::UpdateMovement wraps them to.
float WrapRadians(double degrees) {
    double r=std::fmod(degrees/kDegreesD,kTwoPi);
    if (r<0) r+=kTwoPi;
    return static_cast<float>(r);
}
double YawDegrees(float radians) {return static_cast<double>(radians)*kDegreesD;}
// Stored wrapped to [0,2pi); reported in (-180,180] like the player's pitch.
double PitchDegrees(float radians) {
    const double r=static_cast<double>(radians);
    return (r>std::numbers::pi?r-kTwoPi:r)*kDegreesD;
}
Vec3 ToVec(const RE::NiPoint3& p) {return {p.x,p.y,p.z};}
RE::FreeCameraState* FreeState(RE::PlayerCamera& camera) {
    return static_cast<RE::FreeCameraState*>(camera.cameraStates[RE::CameraState::kFree].get());
}
struct DriverState {
    std::mutex mutex;
    CameraDriver driver;
    void (*original)(RE::FreeCameraState*)=nullptr;
};
DriverState& Driver() {static auto* const state=new DriverState;return *state;}
// FreeCameraState::Update: the engine moves the free camera by its input, then
// builds the camera root from the state's rotation and translation. A path writes
// its pose first, so that frame renders exactly the driven pose.
void HookedFreeCameraUpdate(RE::FreeCameraState* state) {
    auto& d=Driver();
    CameraDriver driver;
    {std::scoped_lock lock(d.mutex);driver=d.driver;}
    if (driver&&state) {
        try {
            if (const auto pose=driver()) {
                state->translation={pose->position.x,pose->position.y,pose->position.z};
                state->rotation.x=WrapRadians(pose->pitch);
                state->rotation.y=WrapRadians(pose->yaw);
            }
        } catch (...) {}
    }
    d.original(state);
}
}

CameraSnapshot NativeWorldServices::SampleCamera() {
    CameraSnapshot s;
    auto* camera=RE::PlayerCamera::GetSingleton();
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (!camera||!player) return s;
    s.sampled=true;
    s.mode=Camera().mode;
    if (camera->cameraRoot) s.cameraPosition=ToVec(camera->cameraRoot->GetWorldTranslate());
    if (auto* free=FreeState(*camera)) {
        s.free.position=ToVec(free->translation);
        s.free.yaw=YawDegrees(free->rotation.y);
        s.free.pitch=PitchDegrees(free->rotation.x);
    }
    s.player.position=ToVec(player->data.location);
    s.player.yaw=static_cast<double>(player->data.angle.z)*kDegreesD;
    s.player.pitch=static_cast<double>(player->data.angle.x)*kDegreesD;
    s.worldFov=camera->worldFOV;
    s.firstPersonFov=camera->firstPersonFOV;
    s.autoVanityAllowed=camera->allowAutoVanityMode;
    return s;
}
bool NativeWorldServices::SetFreeCameraPose(const CameraPose& pose,std::string& error) {
    auto* camera=RE::PlayerCamera::GetSingleton();
    if (!camera||!camera->QCameraEquals(RE::CameraState::kFree)) {error="The free camera is not active";return false;}
    auto* free=FreeState(*camera);
    if (!free) {error="Free camera state unavailable";return false;}
    free->translation={pose.position.x,pose.position.y,pose.position.z};
    free->rotation.x=WrapRadians(pose.pitch);
    free->rotation.y=WrapRadians(pose.yaw);
    return true;
}
bool NativeWorldServices::SetPlayerAngles(double yawDeg,double pitchDeg,std::string& error) {
    auto* player=RE::PlayerCharacter::GetSingleton();
    if (!player) {error="Player unavailable";return false;}
    if (pitchDeg<-89.9||pitchDeg>89.9) {error="pitch must be within -89.9..89.9 degrees";return false;}
    double yaw=std::fmod(yawDeg,360.0);
    if (yaw<0) yaw+=360.0;
    player->SetAngleOnReference(RE::NiPoint3{static_cast<float>(pitchDeg/kDegreesD),player->data.angle.y,static_cast<float>(yaw/kDegreesD)});
    return true;
}
bool NativeWorldServices::SetCameraDriver(CameraDriver driver,std::string& error) {
    auto& d=Driver();
    if (!driver) {std::scoped_lock lock(d.mutex);d.driver={};return true;}
    if (!d.original) {
        auto* camera=RE::PlayerCamera::GetSingleton();
        auto* free=camera?FreeState(*camera):nullptr;
        if (!free) {error="Free camera state unavailable";return false;}
        auto* table=*reinterpret_cast<std::uintptr_t**>(free);
        const auto slot=ids::Camera::FreeCameraUpdateSlot;
        if (!Chainable(table[slot],ids::Camera::FreeCameraUpdate.address())) {
            error="FreeCameraState::Update is not the expected engine function or another plugin's hook (the runtime differs)";
            return false;
        }
        d.original=reinterpret_cast<decltype(d.original)>(table[slot]);
        if (!PatchSlot(table,slot,reinterpret_cast<std::uintptr_t>(&HookedFreeCameraUpdate))) {d.original=nullptr;error="vtable is not writable";return false;}
        REX::INFO("FusionMCP free camera driver hook installed on FreeCameraState::Update");
    }
    std::scoped_lock lock(d.mutex);
    d.driver=std::move(driver);
    return true;
}
void NativeWorldServices::SetAutoVanityAllowed(bool allowed) {
    if (auto* camera=RE::PlayerCamera::GetSingleton()) camera->allowAutoVanityMode=allowed;
}
bool NativeWorldServices::RequestQuit(std::string& error) {
    auto* main=RE::Main::GetSingleton();
    if (!main) {error="Main is unavailable";return false;}
    // What Main::RequestQuitGame and the QuitGame console command set: the main loop exits cleanly.
    main->quitGame=true;
    REX::INFO("FusionMCP game.quit: requested the engine's clean quit");
    return true;
}
}
