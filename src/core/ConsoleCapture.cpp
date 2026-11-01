#include "ConsoleCapture.h"

namespace fusionmcp {
std::uint64_t ConsoleCapture::Begin() {
    std::scoped_lock lock(mutex_);
    text_.clear();truncated_=false;appends_=0;
    open_.store(true,std::memory_order_release);
    return ++generation_;
}
void ConsoleCapture::End(std::uint64_t generation) {
    std::scoped_lock lock(mutex_);
    if (generation==generation_) open_.store(false,std::memory_order_release);
}
void ConsoleCapture::Append(std::string_view text) {
    if (text.empty()||!open_.load(std::memory_order_acquire)) return;
    std::scoped_lock lock(mutex_);
    if (!open_.load(std::memory_order_relaxed)) return;
    ++appends_;
    const auto room=kMaxBytes-text_.size();
    if (text.size()>room) {text=text.substr(0,room);truncated_=true;}
    text_.append(text);
}
ConsoleCapture::Snapshot ConsoleCapture::Read(std::uint64_t generation) const {
    std::scoped_lock lock(mutex_);
    if (!generation||generation!=generation_) return {};
    return {true,open_.load(std::memory_order_relaxed),text_,truncated_,appends_};
}
}
