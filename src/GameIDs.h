#pragma once
// Every game address and struct offset FusionMCP uses that CommonLibF4 does not
// provide. Nothing else in src/ holds REL::ID, REL::VariantID, RVAs or raw
// offsets. VariantID{ og, ae }: og = 1.10.163, ae = the NG/AE id space shared by
// 1.11.221 and 1.11.240. Sources are Address Library rows in the RE symbol
// database (`modkit re al --rva <rva> --runtime <version>`), checked against the
// disassembly of each supported runtime. OG ids are the 1.10.163 rows; none needed
// a bridge from the 1.10.155 PDB.
#include <REL/VariantID.h>
#include <REX/FModule.h>
#include <cstddef>

namespace fusionmcp::ids {
// A member offset that differs between OG and NG/AE.
struct FieldOffset {
    std::size_t og,ae;
    [[nodiscard]] std::size_t value() const noexcept {return REX::FModule::IsRuntimeOG()?og:ae;}
};
// Distance of a rel32 call instruction from the start of its function.
struct CallSite {
    REL::VariantID function;
    FieldOffset offset;
    [[nodiscard]] std::uintptr_t address() const {return function.address()+offset.value();}
};

namespace Input {
    // RE::VTABLE::ButtonEvent; redb AL row, same id in both spaces.
    inline constexpr REL::VariantID ButtonEventVtable{ 1072871, 1072871 };
    // PlayerControls::PerformInputProcessing (BSInputEventReceiver slot 0); redb AL.
    inline constexpr REL::VariantID PerformInputProcessing{ 663896, 2234814 };
}
namespace AnimEvents {
    // Actor / PlayerCharacter BSTEventSink<BSAnimationGraphEvent> vtables (+0x38 subobject); redb AL.
    inline constexpr REL::VariantID ActorSinkVtable{ 720550, 720550 };
    inline constexpr REL::VariantID PlayerSinkVtable{ 1542933, 1542933 };
    // Actor::ProcessEvent(BSAnimationGraphEvent), slot 1 of both sinks; redb AL.
    inline constexpr REL::VariantID ActorProcessEvent{ 754039, 2230469 };
    // Offset of that sink subobject inside TESObjectREFR (vtable listing; all runtimes).
    inline constexpr std::size_t SinkInReference=0x38;
}
namespace Behavior {
    // BShkbAnimationGraph::CharacterInstance (hkbCharacter) and ::sphkbBehaviorGraph; redb struct (1.10.155 PDB),
    // same offsets as Tales PlayerFearEffect and fusion-tools LocomotionProbe use on OG 1.10.163 and AE.
    inline constexpr std::size_t GraphCharacter=0x1C8, GraphRootBehavior=0x378;
    // hkbNode::m_name (hkStringPtr, bit 0 = owns flag) and ::m_type; redb struct, and the
    // `cmp byte [node+0x43],4` / `[node+0x90]` / `[[node+0xD0]+0x10]` reads of ClipTimeVisitor::Visit
    // (163 0x81BEA0, 240 0x817FE0) on both runtimes.
    inline constexpr std::size_t NodeName=0x38, NodeType=0x43;
    // hkbBehaviorReferenceGenerator::m_behaviorName; redb struct.
    inline constexpr std::size_t BehaviorReferenceName=0x88;
    // hkbClipGenerator::m_animationName, m_playbackSpeed, m_animationControl; redb struct + ClipTimeVisitor::Visit.
    inline constexpr std::size_t ClipAnimationName=0x90, ClipPlaybackSpeed=0xB0, ClipAnimationControl=0xD0;
    // hkaAnimationControl::m_localTime, m_weight; redb struct + ClipTimeVisitor::Visit (+0x10).
    inline constexpr std::size_t ControlLocalTime=0x10, ControlWeight=0x14;
    // hkbNodeType reflection enum values.
    inline constexpr std::uint8_t NodeTypeBehaviorReference=2, NodeTypeClipGenerator=4;
    // GetChildrenFlagBits::FLAG_ACTIVE_ONLY, what FO4's GetAnimTimeFunctor passes.
    inline constexpr std::int32_t ActiveChildrenOnly=1;
}
namespace AIProcess {
    // Pure reads (OG disassembly: middleHigh+0x348 dword != 0, high+0x594 byte); redb AL rows
    // (163 0xE38F20 / 240 0xCFE630, 163 0xE38FF0 / 240 0xCFE6D0; 221 shares the NG ids).
    inline constexpr REL::VariantID CurrentlyLoadingWeaponSet{ 824807, 2231760 };
    inline constexpr REL::VariantID IsItemEquipQueued{ 199713, 2231763 };
}
namespace WeaponCull {
    // Not called. The engine paths that drive weaponCullCounter (bit 0 animation, bit 1 switch,
    // bits 2+ counter), resolved for actor.weapon_cull_state's decode; redb AL rows
    // (155 -> 163 -> 221/240). WeaponDrawHandler AE: slot 1 of the WeaponDrawHandler vtable
    // (AL 1378656, 240 0x2573FD8) calls 240 0xC56B50, which matches the OG body
    // (SetWeaponBonesCulled type 1 then 2, ProcessWeaponDrawnChange).
    inline constexpr REL::VariantID HandleItemEquip{ 164912, 2229781 };              // 163 0xD7EB20, 240 0xC686D0
    inline constexpr REL::VariantID WeaponDrawHandler{ 222070, 2229545 };            // 163 0xD6DC70, 240 0xC56B50
    inline constexpr REL::VariantID PollItemEquip{ 612104, 2230558 };                // 163 0xDC4C10, 240 0xCB37A0
    inline constexpr REL::VariantID SetWeaponBonesCulledHelper{ 1110371, 2232667 };  // 163 0xE87780, 240 0xD4C250
}
namespace MessageBoxMenu {
    // MessageBoxMenu::OnButtonPress(menu, u8); redb AL.
    inline constexpr REL::VariantID OnButtonPress{ 466970, 4486575 };
    // MessageBoxMenu::currentMessage; disassembly of OnButtonPress per runtime.
    inline constexpr FieldOffset CurrentMessage{ 0xE8, 0xF0 };
    // MessageBoxData: BSString header and body, BSTArray<BSString> buttons (data, u32 size);
    // disassembly of OnButtonPress, identical on all runtimes.
    inline constexpr std::size_t DataHeader=0x18, DataBody=0x28, DataButtons=0x38, DataButtonCount=0x48;
}
namespace UI {
    // UI::menuSystemVisible, the flag ToggleMenus flips; disassembly per runtime.
    inline constexpr FieldOffset MenuSystemVisible{ 0x248, 0x258 };
}
namespace Camera {
    // PlayerCamera::Force1stPerson / Force3rdPerson; redb AL.
    inline constexpr REL::VariantID Force1stPerson{ 110083, 2248340 };
    inline constexpr REL::VariantID Force3rdPerson{ 162201, 2248348 };
    // FreeCameraState::Update (TESCameraState vtable slot 11): UpdateMovement, then the
    // camera root from GetRotation/GetTranslation; redb AL and vtable listing.
    inline constexpr REL::VariantID FreeCameraUpdate{ 541110, 2248280 };
    inline constexpr std::size_t FreeCameraUpdateSlot=11;
}
namespace Console {
    // ConsoleLog::AddString and its callers; redb AL, call offsets from `modkit re rva`.
    inline constexpr REL::VariantID AddString{ 764, 2248593 };
    // ConsoleLog::Print (og 799546; ae 2248591, which is also PrintLine there).
    inline constexpr CallSite PrintAddStringCall{ REL::VariantID{ 799546, 2248591 }, { 0x5F, 0xA0 } };
    // AE only: the second AddString caller, AL_2248592 (the va_list form). OG has none (offset 0 = absent): OG 1.10.163 ConsoleLog::Print
    // 0x1262EC0 (799546) is that va_list form, the variadic PrintLine 0x1262E90 tail-calls it, and its AddString call (+0x5F, redb xrefs and
    // disassembly) is the sole OG caller of AddString and already PrintAddStringCall above, so AE's two sites collapse to one on OG.
    inline constexpr CallSite SecondAddStringCall{ REL::VariantID{ 0, 2248592 }, { 0, 0x9E } };
    // The variadic print the console echo calls: og PrintLine 1065300, ae 2248591.
    inline constexpr REL::VariantID PrintLine{ 1065300, 2248591 };
    // Console::ExecuteCommand's call that echoes the typed line into PrintLine.
    inline constexpr CallSite ExecuteCommandEchoCall{ REL::VariantID{ 1061864, 2248537 }, { 0x76, 0x89 } };
    inline constexpr REL::VariantID ExecuteQueuedCommand{ 675475, 2248538 };
}
}
