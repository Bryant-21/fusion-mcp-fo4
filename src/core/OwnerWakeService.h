#pragma once
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace fusionmcp {
// Engine-free worker posts trusted owner wakes. It never runs game/render/UI/VM
// operations itself. Transport disconnect/stop does not strand retirement leases.
class OwnerWakeService {
public:
    using Tick=std::function<void()>;
    using HasPending=std::function<bool()>;
    ~OwnerWakeService();
    bool Start(Tick tick,HasPending pending) noexcept;
    void BeginDrain() noexcept;
    bool Stop() noexcept; // Join outside loader lock, never from an owner callback.
    bool Running() const noexcept {return running_.load(std::memory_order_acquire);}
private:
    std::mutex lifecycle_,waitMutex_;
    std::condition_variable wake_;
    std::thread worker_;
    std::atomic_bool stop_{false},drain_{false},running_{false};
};
}
