#include "ShutdownJoinService.h"
namespace fusionmcp {
ShutdownJoinService::~ShutdownJoinService() {Stop();}
bool ShutdownJoinService::Start(Join join) noexcept {
    try {
        std::scoped_lock lock(lifecycle_);
        if (worker_.joinable()||finished_.load(std::memory_order_acquire)||!join) return false;
        succeeded_=false;
        worker_=std::thread([this,join=std::move(join)]() noexcept {
            signal_.wait(0,std::memory_order_acquire);
            bool success=false;
            if (signal_.load(std::memory_order_acquire)==1) try {success=join();} catch (...) {}
            succeeded_.store(success,std::memory_order_release);
            finished_.store(true,std::memory_order_release);
        });
        return true;
    } catch (...) {return false;}
}
void ShutdownJoinService::Request() noexcept {
    // No lifecycle lock, network wait or join on a caller/owner thread.
    unsigned idle=0;signal_.compare_exchange_strong(idle,1,std::memory_order_release);
    signal_.notify_all();
}
bool ShutdownJoinService::Stop() noexcept {
    try {
        std::scoped_lock lock(lifecycle_);
        if (worker_.joinable()&&worker_.get_id()==std::this_thread::get_id()) return false;
        unsigned idle=0;signal_.compare_exchange_strong(idle,2,std::memory_order_release);
        signal_.notify_all();
        if (worker_.joinable()) worker_.join();
        return true;
    } catch (...) {return false;}
}
}
