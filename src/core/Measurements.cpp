#include "core/Measurements.h"
#include "core/FrameCapture.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numeric>
#include <set>

namespace fusionmcp {
namespace {
bool Finite(double v) {return std::isfinite(v);}
double SrgbToLinear(double v) {return v<=0.04045?v/12.92:std::pow((v+0.055)/1.055,2.4);}
// SMPTE ST 2084 to absolute nits.
double PqToNits(double v) {
    constexpr double m1=2610.0/16384,m2=2523.0/4096*128,c1=3424.0/4096,c2=2413.0/4096*32,c3=2392.0/4096*32;
    const double p=std::pow(std::max(v,0.0),1.0/m2);
    return 10000.0*std::pow(std::max(p-c1,0.0)/(c2-c3*p),1.0/m1);
}
double Decode(Transfer t,double v) {
    switch (t) {
    case Transfer::Srgb: return SrgbToLinear(v);
    case Transfer::Pq: return PqToNits(v);
    default: return v;
    }
}
double Round(double v,double scale) {return std::round(v*scale)/scale;}
}

const char* TransferName(Transfer t) noexcept {return t==Transfer::Pq?"pq":t==Transfer::Linear?"linear":"srgb";}

std::optional<std::string> ParseStatsOptions(const Json& args,StatsOptions& o) {
    o=StatsOptions{};
    if (args.contains("regions")) {
        const auto& list=args["regions"];
        if (!list.is_array()||list.empty()||list.size()>kMaxStatsRegions) return "regions holds 1.."+std::to_string(kMaxStatsRegions)+" entries";
        std::set<std::string> names;
        for (const auto& r:list) {
            if (!r.is_object()||!r.contains("name")||!r["name"].is_string()) return "Each region needs a name";
            StatsRegion region{r["name"].get<std::string>()};
            if (region.name.empty()||region.name.size()>40) return "Region names are 1..40 characters";
            if (!names.insert(region.name).second) return "Region names must be unique";
            for (const auto& [key,slot]:{std::pair{"x",&region.x},std::pair{"y",&region.y},std::pair{"w",&region.w},std::pair{"h",&region.h}}) {
                if (!r.contains(key)||!r[key].is_number()||!Finite(r[key].get<double>())) return "Region "+region.name+" needs numeric x, y, w and h";
                *slot=r[key].get<double>();
            }
            if (region.x<0||region.y<0||region.w<=0||region.h<=0||region.x+region.w>1+1e-9||region.y+region.h>1+1e-9)
                return "Region "+region.name+" must lie inside the normalized frame (0..1) with positive size";
            o.regions.push_back(std::move(region));
        }
    } else o.regions.push_back({"full",0,0,1,1});
    if (args.contains("reference")) {
        const auto& ref=args["reference"];
        if (ref.is_string()) {
            o.reference=ref.get<std::string>();
            if (std::none_of(o.regions.begin(),o.regions.end(),[&](const StatsRegion& r){return r.name==o.reference;}))
                return "reference names no region";
        } else if (ref.is_number()&&ref.get<double>()>0&&Finite(ref.get<double>())) o.referenceLuminance=ref.get<double>();
        else return "reference is a region name or a positive linear luminance";
    }
    for (const auto& [key,slot]:{std::pair{"whiteThreshold",&o.whiteThreshold},std::pair{"blackThreshold",&o.blackThreshold}}) {
        if (!args.contains(key)) continue;
        const double v=args[key].get<double>();
        if (!Finite(v)||v<0||v>1) return std::string(key)+" must be within 0..1";
        *slot=v;
    }
    if (args.contains("transfer")) {
        const auto t=args["transfer"].get<std::string>();
        if (t=="srgb") o.transfer=Transfer::Srgb;
        else if (t=="pq") o.transfer=Transfer::Pq;
        else if (t=="linear") o.transfer=Transfer::Linear;
        else return "transfer is srgb, pq or linear";
    }
    return std::nullopt;
}

FrameStats ComputeFrameStats(const RawFrame& frame,const StatsOptions& options) {
    FrameStats out;
    out.transfer=options.transfer.value_or(Transfer::Srgb);
    const bool tenBit=frame.layout==PixelLayout::Rgb10A2;
    const std::size_t levels=tenBit?1024:256;
    // Decoding through a table: one entry per code value.
    std::vector<double> linear(levels);
    for (std::size_t i=0;i<levels;++i) linear[i]=Decode(out.transfer,double(i)/double(levels-1));
    const auto white=static_cast<std::uint32_t>(std::ceil(options.whiteThreshold*double(levels-1)-1e-9));
    const auto black=static_cast<std::uint32_t>(std::floor(options.blackThreshold*double(levels-1)+1e-9));
    const bool usable=frame.width&&frame.height&&frame.texels.size()==std::size_t(frame.width)*frame.height*4&&
        frame.layout!=PixelLayout::Unsupported;
    for (const auto& region:options.regions) {
        RegionStats s;s.name=region.name;
        if (usable) {
            s.x0=std::min(frame.width-1,static_cast<std::uint32_t>(std::floor(region.x*frame.width)));
            s.y0=std::min(frame.height-1,static_cast<std::uint32_t>(std::floor(region.y*frame.height)));
            s.x1=std::clamp(static_cast<std::uint32_t>(std::ceil((region.x+region.w)*frame.width-1e-9)),s.x0+1,frame.width);
            s.y1=std::clamp(static_cast<std::uint32_t>(std::ceil((region.y+region.h)*frame.height-1e-9)),s.y0+1,frame.height);
        }
        double sumR=0,sumG=0,sumB=0;
        std::uint64_t whites=0,blacks=0;
        for (std::uint32_t y=s.y0;y<s.y1;++y) {
            const auto* row=frame.texels.data()+(std::size_t(y)*frame.width+s.x0)*4;
            for (std::uint32_t x=s.x0;x<s.x1;++x,row+=4) {
                std::uint32_t r,g,b;
                if (tenBit) {
                    std::uint32_t texel;std::memcpy(&texel,row,4);
                    r=texel&0x3FF;g=(texel>>10)&0x3FF;b=(texel>>20)&0x3FF;
                } else if (frame.layout==PixelLayout::Bgra8) {b=row[0];g=row[1];r=row[2];}
                else {r=row[0];g=row[1];b=row[2];}
                sumR+=linear[r];sumG+=linear[g];sumB+=linear[b];
                const auto peak=std::max({r,g,b});
                if (peak>=white) ++whites;
                if (peak<=black) ++blacks;
            }
        }
        s.pixels=std::uint64_t(s.x1-s.x0)*(s.y1-s.y0);
        if (s.pixels) {
            const double n=double(s.pixels);
            s.meanR=sumR/n;s.meanG=sumG/n;s.meanB=sumB/n;
            s.meanLuminance=0.2126*s.meanR+0.7152*s.meanG+0.0722*s.meanB;
            if (s.meanG>0) s.rgRatio=s.meanR/s.meanG;
            s.nearWhitePct=100.0*double(whites)/n;s.nearBlackPct=100.0*double(blacks)/n;
        }
        out.regions.push_back(std::move(s));
    }
    std::optional<double> reference=options.referenceLuminance;
    if (!options.reference.empty())
        for (const auto& s:out.regions) if (s.name==options.reference) reference=s.meanLuminance;
    if (reference&&*reference>0)
        for (auto& s:out.regions) if (s.meanLuminance>0) s.ev=std::log2(s.meanLuminance/ *reference);
    return out;
}

Json ToJson(const FrameStats& stats) {
    Json regions=Json::array();
    for (const auto& s:stats.regions) {
        regions.push_back({{"name",s.name},{"rect",{{"x0",s.x0},{"y0",s.y0},{"x1",s.x1},{"y1",s.y1}}},{"pixels",s.pixels},
            {"meanLuminance",Round(s.meanLuminance,1e6)},
            {"meanRGB",{Round(s.meanR,1e6),Round(s.meanG,1e6),Round(s.meanB,1e6)}},
            {"rgRatio",s.rgRatio?Json(Round(*s.rgRatio,1e5)):Json()},{"ev",s.ev?Json(Round(*s.ev,1e4)):Json()},
            {"nearWhitePct",Round(s.nearWhitePct,1e3)},{"nearBlackPct",Round(s.nearBlackPct,1e3)}});
    }
    return {{"transfer",TransferName(stats.transfer)},
        {"luminanceUnit",stats.transfer==Transfer::Pq?"nits":"relative (1.0 = display white)"},{"regions",std::move(regions)}};
}

DurationStats Summarize(std::vector<double> samples) {
    DurationStats s;
    std::erase_if(samples,[](double v){return !std::isfinite(v)||v<0;});
    s.count=samples.size();
    if (samples.empty()) return s;
    std::sort(samples.begin(),samples.end());
    const auto rank=[&](double p) {
        const auto index=static_cast<std::size_t>(std::ceil(p*double(samples.size())));
        return samples[std::clamp<std::size_t>(index,1,samples.size())-1];
    };
    s.meanMs=std::accumulate(samples.begin(),samples.end(),0.0)/double(samples.size());
    s.p50Ms=rank(0.50);s.p99Ms=rank(0.99);s.p999Ms=rank(0.999);
    s.minMs=samples.front();s.maxMs=samples.back();
    return s;
}
Json ToJson(const DurationStats& s) {
    return {{"count",s.count},{"meanMs",Round(s.meanMs,1e4)},{"p50Ms",Round(s.p50Ms,1e4)},{"p99Ms",Round(s.p99Ms,1e4)},
        {"p999Ms",Round(s.p999Ms,1e4)},{"maxMs",Round(s.maxMs,1e4)},{"minMs",Round(s.minMs,1e4)}};
}
}
