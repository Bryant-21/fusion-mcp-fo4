#pragma once
#include <atomic>
#include <functional>
#include <mutex>
#include <thread>
namespace fusionmcp {
// Owner callers only Request(). A dedicated non-owner worker performs the
// potentially blocking transport join. No owner or loader-lock joins are allowed.
class ShutdownJoinService {
public:
    using Join=std::function<bool()>;
    ~ShutdownJoinService();
    bool Start(Join join) noexcept;
    void Request() noexcept;
    bool Finished() const noexcept {return finished_.load(std::memory_order_acquire);}
    bool Succeeded() const noexcept {return Finished()&&succeeded_.load(std::memory_order_acquire);}
    bool Stop() noexcept; // Test/off-owner destruction only; may join this worker.
private:
    std::mutex lifecycle_;
    std::thread worker_;
    // 0 idle, 1 shutdown requested, 2 stop an unused worker. Atomic wait avoids
    // a notify/predicate race without taking a wait lock on an owner caller.
    std::atomic<unsigned> signal_{0};
    std::atomic_bool finished_{false},succeeded_{false};
};
}
