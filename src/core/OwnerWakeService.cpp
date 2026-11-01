#include "OwnerWakeService.h"
#include <chrono>
namespace fusionmcp {
OwnerWakeService::~OwnerWakeService() {Stop();}
bool OwnerWakeService::Start(Tick tick,HasPending pending) noexcept {
    try {
        std::scoped_lock lock(lifecycle_);
        if (worker_.joinable()||!tick||!pending) return false;
        stop_=false;drain_=false;running_=true;
        try {
            worker_=std::thread([this,tick=std::move(tick),pending=std::move(pending)]() noexcept {
                while (!stop_.load(std::memory_order_acquire)) {
                    try {
                        const bool work=pending();
                        if (drain_.load(std::memory_order_acquire)&&!work) break;
                        if (work) tick();
                    } catch (...) { /* Preserve a lease; a later wake may recover. */ }
                    std::unique_lock wait(waitMutex_);
                    // Bounded cadence, independent of client traffic. No work
                    // or join occurs while holding lifecycle or owner locks.
                    if (!stop_.load(std::memory_order_acquire)) wake_.wait_for(wait,std::chrono::milliseconds(100));
                }
                running_.store(false,std::memory_order_release);
            });
        } catch (...) {running_=false;return false;}
        return true;
    } catch (...) {return false;}
}
void OwnerWakeService::BeginDrain() noexcept {drain_.store(true,std::memory_order_release);wake_.notify_all();}
bool OwnerWakeService::Stop() noexcept {
    try {
        std::scoped_lock lock(lifecycle_);
        if (worker_.joinable()&&worker_.get_id()==std::this_thread::get_id()) return false;
        stop_.store(true,std::memory_order_release);wake_.notify_all();
        if (worker_.joinable()) worker_.join();
        return true;
    } catch (...) {return false;}
}
}
