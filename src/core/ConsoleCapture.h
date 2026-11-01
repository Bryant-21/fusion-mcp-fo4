#pragma once
// Project-owned GPL-3.0 code. Engine-free capture of console print text, bounded
// to one console line at a time. The native adapter feeds it from its print hook.
#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>

namespace fusionmcp {
class ConsoleCapture {
public:
    static constexpr std::size_t kMaxBytes=65536;
    struct Snapshot {
        bool current=false;      // false: a later Begin superseded this capture, or it never existed
        bool open=false;         // still recording
        std::string text;
        bool truncated=false;
        std::uint64_t appends=0; // print calls recorded, including truncated ones
    };
    // Starts recording for a new line; the previous capture is discarded.
    std::uint64_t Begin();
    void End(std::uint64_t generation);
    // Any thread. Ignored unless a capture is open.
    void Append(std::string_view text);
    Snapshot Read(std::uint64_t generation) const;
private:
    mutable std::mutex mutex_;
    std::atomic_bool open_{false};
    std::uint64_t generation_=0;
    std::string text_;
    bool truncated_=false;
    std::uint64_t appends_=0;
};
}
