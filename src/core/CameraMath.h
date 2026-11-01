#pragma once
// Project-owned GPL-3.0 code. Engine-free camera poses, keyframe/walk paths and
// placement offsets for fusionmcp.camera.* and fusionmcp.world.place. Angles follow the
// engine's reference angles (see CameraPose).
#include "core/Json.h"
#include "core/WorldServices.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fusionmcp {
double WrapYaw(double degrees);                    // [0,360)
Vec3 Forward(double yaw,double pitch);             // unit view direction
void DirectionAngles(const Vec3& direction,double& yaw,double& pitch);
// offset = (forward, right, up). forward follows pitch when withPitch; up is world +Z.
Vec3 OffsetPosition(const Vec3& origin,double yaw,double pitch,const Vec3& offset,bool withPitch);
std::optional<Vec3> ParseVec3(const Json& value);
Json ToJson(const Vec3& v);

enum class Interpolation {Linear,Slerp};
// linear: position, yaw (shortest way round) and pitch interpolate independently.
// slerp: the view direction moves along the great circle between the two directions.
CameraPose Interpolate(const CameraPose& a,const CameraPose& b,double t,Interpolation mode);

struct PathShot {std::string name;double atMs=0;};
struct CameraPath {
    struct Key {double atMs=0;CameraPose pose;};
    std::vector<Key> keys;                         // ascending, first at 0
    Interpolation interpolation=Interpolation::Linear;
    // Walk: from keys[0] at speed units/s, heading turning at yawRate deg/s; height and pitch stay.
    bool walk=false;
    double speed=0,yawRate=0;
    double durationMs=0;
    std::vector<PathShot> shots;
    std::optional<double> frameStepMs;             // path time per frame instead of wall time
    CameraPose At(double ms) const;
};
inline constexpr double kMaxPathMs=120000;
inline constexpr std::size_t kMaxPathKeys=64,kMaxPathShots=32;
// fusionmcp.camera.path arguments; start fills the walk start and fields a keyframe leaves out.
std::optional<std::string> ParseCameraPath(const Json& args,const CameraPose& start,CameraPath& path);

// Per-frame progress of a path: the pose for each frame and which shots fall due.
class CameraPathRun {
public:
    enum class ShotState {Waiting,Requested,Done,Failed};
    struct Shot {
        PathShot spec;
        ShotState state=ShotState::Waiting;
        std::uint64_t captureId=0;
        double takenAtPathMs=0,takenAtWallMs=0;
        std::uint64_t frame=0;
        unsigned busyRetries=0;
        Json result;
    };
    struct Step {CameraPose pose;double pathMs=0;std::vector<std::size_t> due;};
    explicit CameraPathRun(CameraPath path);
    // One engine frame; wallMs counts from the path's first frame. Once motion is
    // done it holds the final pose and only offers shots still waiting.
    Step Advance(double wallMs);
    void Requested(std::size_t shot,std::uint64_t captureId,const Step& step,double wallMs);
    void Finished(std::size_t shot,bool ok,Json result);
    bool MotionDone() const noexcept {return motionDone_;}
    bool ShotsSettled() const;
    std::uint64_t Frames() const noexcept {return frames_;}
    double LastWallMs() const noexcept {return lastWallMs_;}
    const std::vector<double>& FrameTimes() const noexcept {return frameTimes_;}
    std::vector<Shot>& Shots() noexcept {return shots_;}
    const CameraPath& Path() const noexcept {return path_;}
private:
    CameraPath path_;
    std::vector<Shot> shots_;
    std::uint64_t frames_=0;
    double lastWallMs_=0;
    bool motionDone_=false;
    std::vector<double> frameTimes_;
};
}
