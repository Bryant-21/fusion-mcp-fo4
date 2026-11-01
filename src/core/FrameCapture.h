#pragma once
// Project-owned GPL-3.0 code. Engine-free half of the native present-time
// screenshot: format mapping, row packing, PNG writing and the request state
// machine shared by the MCP side and the render thread. The capture technique
// (one GPU copy into a staging texture at the present seam, a DO_NOT_WAIT Map on
// later frames, PNG encoding on a worker) follows F4FS's Screenshot tool, itself
// derived from Skyrim Community Shaders; see THIRD_PARTY.md.
#include "core/Artifact.h"
#include "core/GameServices.h"
#include "core/Measurements.h"
#include "core/RequestBroker.h"
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fusionmcp {
enum class PixelLayout {Unsupported,Rgba8,Bgra8,Rgb10A2};
// dxgiFormat is a DXGI_FORMAT value.
PixelLayout ClassifyFormat(std::uint32_t dxgiFormat) noexcept;
// 3 = 24-bit BGR PNG, 6 = 48-bit RGB PNG, 0 = cannot be saved.
std::uint32_t OutputBytesPerPixel(PixelLayout layout) noexcept;
// Converts one tightly packed row of width 4-byte texels into the output row.
void PackRow(PixelLayout layout,const std::uint8_t* source,std::uint32_t width,std::uint8_t* destination) noexcept;

struct RawFrame {
    std::uint32_t width=0,height=0;
    PixelLayout layout=PixelLayout::Unsupported;
    std::uint32_t dxgiFormat=0;
    std::vector<std::uint8_t> texels; // height rows of width*4 bytes, no padding
};
// Blocking WIC encode; never on the Game or Render owner. Removes a partial file on failure.
bool WriteFramePng(const RawFrame& frame,const std::filesystem::path& path,std::string& error);

class FrameCapture {
public:
    struct Limits {
        // No Present at all after the request: the game is not presenting.
        std::chrono::milliseconds firstFrame{3000};
        // Copy issued but never read back: presents stopped after the copy.
        std::chrono::milliseconds readback{3000};
        std::chrono::milliseconds encode{20000};
    };
    enum class State {Pending,Completed,Failed};
    struct Admission {std::uint64_t id=0;std::string code,message;};
    struct Options {
        std::string label;                          // appended to the file name (sanitised)
        std::shared_ptr<const StatsOptions> stats;  // region statistics of the same frame
    };
    struct Status {
        State state=State::Failed;
        std::string code,message,name,path;
        ArtifactInfo artifact;
        std::uint32_t dxgiFormat=0;
        std::uint64_t presentCount=0;
        std::optional<FrameStats> stats;
    };

    FrameCapture(std::filesystem::path directory,RequestBroker::NowFn now,Limits limits={});
    ~FrameCapture();
    FrameCapture(const FrameCapture&)=delete;
    FrameCapture& operator=(const FrameCapture&)=delete;

    // Any thread. The runtime reports whether its present hook is installed.
    void SetInstalled(bool installed,std::string detail);
    bool Installed() const;
    std::string Detail() const;
    const std::filesystem::path& Directory() const noexcept {return directory_;}
    std::uint64_t Presents() const noexcept {return presents_.load(std::memory_order_relaxed);}
    // One capture is in flight at a time; at most kMaxEncodeBacklog frames wait for encoding.
    Admission Request(Options options={});
    Status Poll(std::uint64_t id);
    void Cancel(std::uint64_t id);
    // Filesystem only: the PNGs in Directory().
    std::vector<FileEntry> Files() const;
    // Times of the most recent presents (up to kPresentHistory), oldest first.
    std::vector<RequestTime> PresentsSince(RequestTime since) const;

    // Render thread, once per game Present. Returns the request to copy from
    // this frame, if one is waiting. Cheap when nothing is requested.
    std::optional<std::uint64_t> OnPresent() noexcept;
    // Render thread: the copied pixels (queued for encoding) or why it failed.
    void Deliver(std::uint64_t id,RawFrame frame);
    void Fail(std::uint64_t id,std::string code,std::string message);

    static constexpr std::size_t kMaxEncodeBacklog=2;
    static constexpr std::size_t kRetainedResults=16;
    static constexpr std::size_t kPresentHistory=16384;
private:
    enum class Phase {Waiting,Copying,Encoding,Done};
    struct Entry {
        Phase phase=Phase::Waiting;
        RequestTime since{};
        std::uint64_t presentsAtRequest=0;
        Options options;
        Status status;
    };
    struct Job {std::uint64_t id=0;RawFrame frame;std::filesystem::path path;std::shared_ptr<const StatsOptions> stats;};
    void Finish(Entry& entry,State state,std::string code,std::string message);
    void Retire();
    void Work(std::stop_token stop);
    std::string NotPresentingMessage(const Entry& entry,RequestTime now) const;

    std::filesystem::path directory_;
    RequestBroker::NowFn now_;
    Limits limits_;
    std::atomic<std::uint64_t> presents_{0};
    std::atomic<std::int64_t> lastPresentTicks_{0};
    std::atomic<bool> waiting_{false};
    mutable std::mutex historyMutex_;
    std::vector<RequestTime> history_;
    std::size_t historyNext_=0;
    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    bool installed_=false;
    std::string detail_="present hook not installed";
    std::uint64_t nextId_=1;
    std::map<std::uint64_t,Entry> entries_;
    std::deque<Job> jobs_;
    std::size_t encoding_=0;
    std::jthread worker_;
};
}
