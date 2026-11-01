#pragma once
// Project-owned GPL-3.0 code. CommonLibF4 world adapter; DLL only. Every method
// runs on the Game owner (main thread); the input hook runs inside the engine's
// own per-frame PlayerControls input pass, also on the main thread.
#include "core/WorldServices.h"
#include <chrono>
#include <cstdint>
#include <map>
#include <mutex>
#include <string>
#include <vector>

namespace fusionmcp::runtime {
// Index into per-runtime RVA tables: 0 = 1.10.163, 1 = 1.11.221, 2 = 1.11.240.
class NativeWorldServices final : public WorldServices {
public:
    explicit NativeWorldServices(int runtimeSlot) noexcept:slot_(runtimeSlot) {}
    FormInfo ResolveForm(const std::string& spec) override;
    RefInfo InspectRef(std::uint32_t formId) override;
    ActorInfo InspectActor(std::uint32_t formId,std::size_t inventoryLimit) override;
    std::vector<RefInfo> Nearby(std::uint32_t anchorId,float radius,const std::string& kind,std::size_t limit) override;
    std::vector<std::string> OpenMenus() override;
    MessageBoxState ReadMessageBox() override;
    bool AnswerMessageBox(std::uint32_t button,std::string& error) override;
    CameraState Camera() override;
    bool SetCameraMode(const std::string& mode,std::string& error) override;
    bool SetLook(float yawDeg,float pitchDeg,bool relative,std::string& error) override;
    bool FaceReference(std::uint32_t formId,std::string& error) override;
    bool SetControl(Control control,bool held,std::string& error) override;
    void ReleaseAllControls() override;
    bool HudVisible() override;
    bool SetHudVisible(bool visible) override;
    std::vector<GraphVariable> GraphVariables(std::uint32_t formId,const std::vector<std::string>& names,std::string& error) override;
    bool StartAnimWatch(std::uint64_t watchId,std::uint32_t formId,std::string& error) override;
    std::vector<AnimEvent> DrainAnimWatch(std::uint64_t watchId) override;
    void StopAnimWatch(std::uint64_t watchId) override;
    GlobalInfo ReadGlobal(std::uint32_t formId) override;
    bool WriteGlobal(std::uint32_t formId,float value,std::string& error) override;
    std::uint32_t GameHourGlobal() override;
    CameraSnapshot SampleCamera() override;
    bool SetFreeCameraPose(const CameraPose& pose,std::string& error) override;
    bool SetPlayerAngles(double yawDeg,double pitchDeg,std::string& error) override;
    bool SetCameraDriver(CameraDriver driver,std::string& error) override;
    void SetAutoVanityAllowed(bool allowed) override;
    bool RequestQuit(std::string& error) override;
    std::optional<NodeInfo> Node(std::uint32_t formId,bool firstPerson,const std::string& node,std::string& error) override;
    std::vector<NodeEntry> NodeChildren(std::uint32_t formId,bool firstPerson,const std::string& node,std::uint32_t maxDepth,
        std::size_t limit,bool& truncated,std::string& error) override;
    std::vector<ActiveClip> ActiveClips(std::uint32_t formId,std::size_t limit,bool& truncated,std::string& error) override;
    WeaponAttachment EquippedWeapon3D(std::uint32_t formId,bool firstPerson) override;
    std::optional<WeaponCullState> WeaponCull(std::uint32_t formId,std::string& error) override;
private:
    bool InstallInputHook(std::string& error);
    int slot_;
};
}
