#include "core/CameraMath.h"
#include <algorithm>
#include <cmath>
#include <numbers>
#include <set>

namespace fusionmcp {
namespace {
constexpr double kRadians=std::numbers::pi/180.0;
double Lerp(double a,double b,double t) {return a+(b-a)*t;}
Vec3 Lerp(const Vec3& a,const Vec3& b,double t) {
    return {static_cast<float>(Lerp(a.x,b.x,t)),static_cast<float>(Lerp(a.y,b.y,t)),static_cast<float>(Lerp(a.z,b.z,t))};
}
// Signed shortest difference b-a in (-180,180].
double YawDelta(double a,double b) {
    double d=std::fmod(b-a,360.0);
    if (d>180) d-=360;
    if (d<=-180) d+=360;
    return d;
}
bool Finite(double v) {return std::isfinite(v)&&std::fabs(v)<1e9;}
bool SafeShotName(const std::string& name) {
    return !name.empty()&&name.size()<=40&&std::all_of(name.begin(),name.end(),[](unsigned char c){return std::isalnum(c)||c=='_'||c=='-';});
}
std::optional<std::string> ParsePose(const Json& value,CameraPose& pose) {
    if (!value.is_object()) return "pose must be an object";
    if (value.contains("position")) {
        const auto p=ParseVec3(value["position"]);
        if (!p) return "position must be {x,y,z} with finite numbers";
        pose.position=*p;
    }
    for (const char* key:{"yaw","pitch"}) if (value.contains(key)) {
        if (!value[key].is_number()||!Finite(value[key].get<double>())) return std::string(key)+" must be a finite number";
    }
    if (value.contains("yaw")) pose.yaw=value["yaw"].get<double>();
    if (value.contains("pitch")) {
        pose.pitch=value["pitch"].get<double>();
        if (pose.pitch<-89.9||pose.pitch>89.9) return "pitch must be within -89.9..89.9 degrees";
    }
    return std::nullopt;
}
}

double WrapYaw(double degrees) {
    double w=std::fmod(degrees,360.0);
    if (w<0) w+=360.0;
    return w>=360.0?0.0:w;
}
Vec3 Forward(double yaw,double pitch) {
    const double y=yaw*kRadians,p=pitch*kRadians;
    return {static_cast<float>(std::sin(y)*std::cos(p)),static_cast<float>(std::cos(y)*std::cos(p)),static_cast<float>(-std::sin(p))};
}
void DirectionAngles(const Vec3& d,double& yaw,double& pitch) {
    const double length=std::sqrt(double(d.x)*d.x+double(d.y)*d.y+double(d.z)*d.z);
    if (length<=0) {yaw=pitch=0;return;}
    yaw=WrapYaw(std::atan2(double(d.x),double(d.y))/kRadians);
    pitch=-std::asin(std::clamp(double(d.z)/length,-1.0,1.0))/kRadians;
}
Vec3 OffsetPosition(const Vec3& origin,double yaw,double pitch,const Vec3& offset,bool withPitch) {
    const auto f=Forward(yaw,withPitch?pitch:0.0);
    const double y=yaw*kRadians;
    const double rx=std::cos(y),ry=-std::sin(y);
    return {static_cast<float>(origin.x+f.x*offset.x+rx*offset.y),static_cast<float>(origin.y+f.y*offset.x+ry*offset.y),
        static_cast<float>(origin.z+f.z*offset.x+offset.z)};
}
std::optional<Vec3> ParseVec3(const Json& value) {
    if (!value.is_object()) return std::nullopt;
    Vec3 v;
    for (const auto& [key,slot]:{std::pair{"x",&v.x},std::pair{"y",&v.y},std::pair{"z",&v.z}}) {
        if (!value.contains(key)||!value[key].is_number()||!Finite(value[key].get<double>())) return std::nullopt;
        *slot=value[key].get<float>();
    }
    return v;
}
Json ToJson(const Vec3& v) {return {{"x",double(v.x)},{"y",double(v.y)},{"z",double(v.z)}};}

CameraPose Interpolate(const CameraPose& a,const CameraPose& b,double t,Interpolation mode) {
    t=std::clamp(t,0.0,1.0);
    CameraPose out;
    out.position=Lerp(a.position,b.position,t);
    if (mode==Interpolation::Linear) {
        out.yaw=WrapYaw(a.yaw+YawDelta(a.yaw,b.yaw)*t);
        out.pitch=Lerp(a.pitch,b.pitch,t);
        return out;
    }
    const auto fa=Forward(a.yaw,a.pitch),fb=Forward(b.yaw,b.pitch);
    const double dot=std::clamp(double(fa.x)*fb.x+double(fa.y)*fb.y+double(fa.z)*fb.z,-1.0,1.0);
    const double angle=std::acos(dot);
    Vec3 d;
    if (angle<1e-6) d=Lerp(fa,fb,t);
    else {
        const double s=std::sin(angle),wa=std::sin((1-t)*angle)/s,wb=std::sin(t*angle)/s;
        d={static_cast<float>(wa*fa.x+wb*fb.x),static_cast<float>(wa*fa.y+wb*fb.y),static_cast<float>(wa*fa.z+wb*fb.z)};
    }
    DirectionAngles(d,out.yaw,out.pitch);
    // Endpoints exactly as given, so a path ends precisely on its last keyframe.
    if (t<=0) {out.yaw=a.yaw;out.pitch=a.pitch;}
    if (t>=1) {out.yaw=b.yaw;out.pitch=b.pitch;}
    return out;
}

CameraPose CameraPath::At(double ms) const {
    ms=std::clamp(ms,0.0,durationMs);
    if (walk) {
        const auto& s=keys.front().pose;
        const double t=ms/1000.0;
        CameraPose p=s;
        p.yaw=WrapYaw(s.yaw+yawRate*t);
        const double y0=s.yaw*kRadians;
        double dx,dy;
        if (std::fabs(yawRate)<1e-9) {dx=speed*t*std::sin(y0);dy=speed*t*std::cos(y0);}
        else {
            const double w=yawRate*kRadians,y1=y0+w*t;
            dx=speed/w*(std::cos(y0)-std::cos(y1));
            dy=speed/w*(std::sin(y1)-std::sin(y0));
        }
        p.position={static_cast<float>(s.position.x+dx),static_cast<float>(s.position.y+dy),s.position.z};
        if (ms<=0) p=s;
        return p;
    }
    if (ms<=keys.front().atMs) return keys.front().pose;
    for (std::size_t i=1;i<keys.size();++i) if (ms<=keys[i].atMs) {
        const auto& a=keys[i-1];const auto& b=keys[i];
        return Interpolate(a.pose,b.pose,(ms-a.atMs)/(b.atMs-a.atMs),interpolation);
    }
    return keys.back().pose;
}

std::optional<std::string> ParseCameraPath(const Json& args,const CameraPose& start,CameraPath& path) {
    path=CameraPath{};
    const bool hasKeys=args.contains("keyframes"),hasWalk=args.contains("walk");
    if (hasKeys==hasWalk) return "Give exactly one of keyframes or walk";
    if (args.value("interpolation",std::string("linear"))=="slerp") path.interpolation=Interpolation::Slerp;
    if (hasKeys) {
        const auto& list=args["keyframes"];
        if (!list.is_array()||list.empty()||list.size()>kMaxPathKeys) return "keyframes must hold 1.."+std::to_string(kMaxPathKeys)+" entries";
        CameraPose previous=start;
        double last=-1;
        for (const auto& item:list) {
            if (!item.is_object()||!item.contains("atMs")||!item["atMs"].is_number()) return "Each keyframe needs atMs";
            const double at=item["atMs"].get<double>();
            if (!Finite(at)||at<0||at>kMaxPathMs) return "keyframe atMs must be within 0.."+std::to_string(int(kMaxPathMs));
            if (at<=last) return "keyframe atMs must strictly increase";
            CameraPose pose=previous;
            if (const auto error=ParsePose(item,pose)) return "keyframe: "+*error;
            if (path.keys.empty()&&at>0) path.keys.push_back({0,start});
            path.keys.push_back({at,pose});
            previous=pose;last=at;
        }
        if (path.keys.size()<2) return "A keyframe path needs two poses or a first keyframe after 0 ms";
        path.durationMs=path.keys.back().atMs;
    } else {
        const auto& walk=args["walk"];
        if (!walk.is_object()) return "walk must be an object";
        CameraPose pose=start;
        if (walk.contains("start")) if (const auto error=ParsePose(walk["start"],pose)) return "walk.start: "+*error;
        if (!walk.contains("durationMs")||!walk["durationMs"].is_number()) return "walk.durationMs is required";
        path.durationMs=walk["durationMs"].get<double>();
        if (!Finite(path.durationMs)||path.durationMs<=0||path.durationMs>kMaxPathMs) return "walk.durationMs must be within 1.."+std::to_string(int(kMaxPathMs));
        path.speed=walk.value("speed",0.0);path.yawRate=walk.value("yawRate",0.0);
        if (!Finite(path.speed)||std::fabs(path.speed)>10000) return "walk.speed must be within -10000..10000 units/s";
        if (!Finite(path.yawRate)||std::fabs(path.yawRate)>720) return "walk.yawRate must be within -720..720 deg/s";
        path.walk=true;path.keys.push_back({0,pose});
    }
    if (args.contains("frameStepMs")) {
        const double step=args["frameStepMs"].get<double>();
        if (!Finite(step)||step<1||step>100) return "frameStepMs must be within 1..100";
        path.frameStepMs=step;
    }
    if (args.contains("shots")) {
        const auto& list=args["shots"];
        if (!list.is_array()||list.size()>kMaxPathShots) return "shots holds at most "+std::to_string(kMaxPathShots)+" entries";
        std::set<std::string> names;
        for (const auto& item:list) {
            if (!item.is_object()||!item.contains("name")||!item["name"].is_string()||!item.contains("atMs")||!item["atMs"].is_number())
                return "Each shot needs name and atMs";
            PathShot shot{item["name"].get<std::string>(),item["atMs"].get<double>()};
            if (!SafeShotName(shot.name)) return "shot names are 1..40 letters, digits, '_' or '-'";
            if (!names.insert(shot.name).second) return "shot names must be unique";
            if (!Finite(shot.atMs)||shot.atMs<0||shot.atMs>path.durationMs) return "shot atMs must lie within the path duration";
            path.shots.push_back(std::move(shot));
        }
        std::stable_sort(path.shots.begin(),path.shots.end(),[](const auto& a,const auto& b){return a.atMs<b.atMs;});
    }
    return std::nullopt;
}

CameraPathRun::CameraPathRun(CameraPath path):path_(std::move(path)) {
    for (const auto& shot:path_.shots) shots_.push_back({shot});
}
CameraPathRun::Step CameraPathRun::Advance(double wallMs) {
    if (motionDone_) {
        Step step{path_.At(path_.durationMs),path_.durationMs,{}};
        for (std::size_t i=0;i<shots_.size();++i) if (shots_[i].state==ShotState::Waiting) step.due.push_back(i);
        return step;
    }
    if (frames_) frameTimes_.push_back(wallMs-lastWallMs_);
    lastWallMs_=wallMs;
    const double pathMs=std::min(path_.durationMs,path_.frameStepMs?double(frames_)*path_.frameStepMs.value():wallMs);
    ++frames_;
    Step step{path_.At(pathMs),pathMs,{}};
    for (std::size_t i=0;i<shots_.size();++i)
        if (shots_[i].state==ShotState::Waiting&&shots_[i].spec.atMs<=pathMs) step.due.push_back(i);
    // The final pose has been applied once motion is done.
    if (pathMs>=path_.durationMs) motionDone_=true;
    return step;
}
void CameraPathRun::Requested(std::size_t i,std::uint64_t captureId,const Step& step,double wallMs) {
    auto& s=shots_.at(i);
    s.state=ShotState::Requested;s.captureId=captureId;s.takenAtPathMs=step.pathMs;s.takenAtWallMs=wallMs;s.frame=frames_;
}
void CameraPathRun::Finished(std::size_t i,bool ok,Json result) {
    auto& s=shots_.at(i);
    s.state=ok?ShotState::Done:ShotState::Failed;s.result=std::move(result);
}
bool CameraPathRun::ShotsSettled() const {
    return std::all_of(shots_.begin(),shots_.end(),[](const Shot& s){return s.state==ShotState::Done||s.state==ShotState::Failed;});
}
}
