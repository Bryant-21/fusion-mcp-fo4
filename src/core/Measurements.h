#pragma once
// Project-owned GPL-3.0 code. Engine-free measurements for fusionmcp.capture.stats
// (per-region image statistics of a read-back frame) and fusionmcp.perf.sample
// (frame time distribution).
#include "core/Json.h"
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace fusionmcp {
struct RawFrame;
enum class Transfer {Srgb,Pq,Linear};
const char* TransferName(Transfer t) noexcept;
struct StatsRegion {std::string name;double x=0,y=0,w=1,h=1;};
struct StatsOptions {
    std::vector<StatsRegion> regions;          // normalized rects; default one "full" region
    std::string reference;                     // region whose luminance is EV 0
    std::optional<double> referenceLuminance;  // or an explicit linear luminance
    double whiteThreshold=0.98,blackThreshold=0.02; // on display-encoded 0..1 max channel
    std::optional<Transfer> transfer;          // default sRGB
};
inline constexpr std::size_t kMaxStatsRegions=16;
std::optional<std::string> ParseStatsOptions(const Json& args,StatsOptions& options);

struct RegionStats {
    std::string name;
    std::uint32_t x0=0,y0=0,x1=0,y1=0;          // pixel bounds, end exclusive
    std::uint64_t pixels=0;
    double meanLuminance=0,meanR=0,meanG=0,meanB=0;
    std::optional<double> rgRatio,ev;
    double nearWhitePct=0,nearBlackPct=0;
};
struct FrameStats {
    Transfer transfer=Transfer::Srgb;
    std::vector<RegionStats> regions;
};
// Blocking pixel loop; never on an engine owner.
FrameStats ComputeFrameStats(const RawFrame& frame,const StatsOptions& options);
Json ToJson(const FrameStats& stats);

struct DurationStats {
    std::size_t count=0;
    double meanMs=0,p50Ms=0,p99Ms=0,p999Ms=0,maxMs=0,minMs=0;
};
// Nearest-rank percentiles.
DurationStats Summarize(std::vector<double> samplesMs);
Json ToJson(const DurationStats& stats);
}
