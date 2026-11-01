#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <dxgiformat.h>
#include "FrameCapture.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>

namespace fusionmcp {
namespace {
std::uint16_t Widen10(std::uint32_t value) noexcept {return static_cast<std::uint16_t>((value<<6)|(value>>4));}
std::string Hex(HRESULT hr) {
    char text[16]{};
    std::snprintf(text,sizeof(text),"0x%08X",static_cast<unsigned>(hr));
    return text;
}
std::string FileName(std::uint64_t id,const std::string& label) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char text[96]{};
    std::snprintf(text,sizeof(text),"FUSIONMCP_%04u-%02u-%02u_%02u-%02u-%02u_%03u_%llu",t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,
        t.wSecond,t.wMilliseconds,static_cast<unsigned long long>(id));
    std::string name=text;
    if (!label.empty()) {
        name+='_';
        for (const char c:label.substr(0,40)) name+=std::isalnum(static_cast<unsigned char>(c))||c=='-'?c:'_';
    }
    return name+".png";
}
long long Milliseconds(RequestTime::duration duration) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(duration).count();
}
}

PixelLayout ClassifyFormat(std::uint32_t dxgiFormat) noexcept {
    switch (static_cast<DXGI_FORMAT>(dxgiFormat)) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        return PixelLayout::Rgba8;
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return PixelLayout::Bgra8;
    case DXGI_FORMAT_R10G10B10A2_UNORM:
        return PixelLayout::Rgb10A2;
    default:
        return PixelLayout::Unsupported;
    }
}
std::uint32_t OutputBytesPerPixel(PixelLayout layout) noexcept {
    switch (layout) {
    case PixelLayout::Rgba8:
    case PixelLayout::Bgra8: return 3;
    case PixelLayout::Rgb10A2: return 6;
    default: return 0;
    }
}
void PackRow(PixelLayout layout,const std::uint8_t* source,std::uint32_t width,std::uint8_t* destination) noexcept {
    switch (layout) {
    case PixelLayout::Rgba8:
        for (std::uint32_t x=0;x<width;++x,source+=4,destination+=3) {destination[0]=source[2];destination[1]=source[1];destination[2]=source[0];}
        break;
    case PixelLayout::Bgra8:
        for (std::uint32_t x=0;x<width;++x,source+=4,destination+=3) {destination[0]=source[0];destination[1]=source[1];destination[2]=source[2];}
        break;
    case PixelLayout::Rgb10A2:
        for (std::uint32_t x=0;x<width;++x,source+=4,destination+=6) {
            std::uint32_t texel=0;
            std::memcpy(&texel,source,sizeof(texel));
            const std::uint16_t rgb[3]={Widen10(texel&0x3FFu),Widen10((texel>>10)&0x3FFu),Widen10((texel>>20)&0x3FFu)};
            std::memcpy(destination,rgb,sizeof(rgb));
        }
        break;
    default:
        break;
    }
}

bool WriteFramePng(const RawFrame& frame,const std::filesystem::path& path,std::string& error) {
    using Microsoft::WRL::ComPtr;
    const auto bytesPerPixel=OutputBytesPerPixel(frame.layout);
    if (!bytesPerPixel) {error="Swap chain format "+std::to_string(frame.dxgiFormat)+" cannot be saved as PNG";return false;}
    if (!frame.width||!frame.height||frame.texels.size()!=std::size_t(frame.width)*frame.height*4) {error="Frame has no pixels";return false;}
    const auto stride=frame.width*bytesPerPixel;
    std::vector<std::uint8_t> packed(std::size_t(stride)*frame.height);
    for (std::uint32_t y=0;y<frame.height;++y)
        PackRow(frame.layout,frame.texels.data()+std::size_t(y)*frame.width*4,frame.width,packed.data()+std::size_t(y)*stride);

    const WICPixelFormatGUID wanted=bytesPerPixel==6?GUID_WICPixelFormat48bppRGB:GUID_WICPixelFormat24bppBGR;
    WICPixelFormatGUID format=wanted;
    // Callable from any thread: COM is joined (or already joined) for the call.
    const HRESULT com=CoInitializeEx(nullptr,COINIT_MULTITHREADED);
    HRESULT hr=S_OK;
    {
        ComPtr<IWICImagingFactory> factory;
        ComPtr<IWICStream> stream;
        ComPtr<IWICBitmapEncoder> encoder;
        ComPtr<IWICBitmapFrameEncode> target;
        ComPtr<IPropertyBag2> options;
        hr=CoCreateInstance(CLSID_WICImagingFactory,nullptr,CLSCTX_INPROC_SERVER,IID_PPV_ARGS(&factory));
        if (SUCCEEDED(hr)) hr=factory->CreateStream(&stream);
        if (SUCCEEDED(hr)) hr=stream->InitializeFromFilename(path.c_str(),GENERIC_WRITE);
        if (SUCCEEDED(hr)) hr=factory->CreateEncoder(GUID_ContainerFormatPng,nullptr,&encoder);
        if (SUCCEEDED(hr)) hr=encoder->Initialize(stream.Get(),WICBitmapEncoderNoCache);
        if (SUCCEEDED(hr)) hr=encoder->CreateNewFrame(&target,&options);
        if (SUCCEEDED(hr)) hr=target->Initialize(options.Get());
        if (SUCCEEDED(hr)) hr=target->SetSize(frame.width,frame.height);
        if (SUCCEEDED(hr)) hr=target->SetPixelFormat(&format);
        if (SUCCEEDED(hr)&&!IsEqualGUID(format,wanted)) hr=WINCODEC_ERR_UNSUPPORTEDPIXELFORMAT;
        if (SUCCEEDED(hr)) hr=target->WritePixels(frame.height,stride,static_cast<UINT>(packed.size()),packed.data());
        if (SUCCEEDED(hr)) hr=target->Commit();
        if (SUCCEEDED(hr)) hr=encoder->Commit();
    }
    if (SUCCEEDED(com)) CoUninitialize();
    if (SUCCEEDED(hr)) return true;
    error="PNG encode failed (hr="+Hex(hr)+")";
    std::error_code ignored;
    std::filesystem::remove(path,ignored);
    return false;
}

FrameCapture::FrameCapture(std::filesystem::path directory,RequestBroker::NowFn now,Limits limits)
    :directory_(std::move(directory)),now_(std::move(now)),limits_(limits),worker_([this](std::stop_token stop){Work(stop);}) {}
FrameCapture::~FrameCapture() {
    worker_.request_stop();
    wake_.notify_all();
}

void FrameCapture::SetInstalled(bool installed,std::string detail) {
    std::scoped_lock lock(mutex_);
    installed_=installed;detail_=std::move(detail);
}
bool FrameCapture::Installed() const {std::scoped_lock lock(mutex_);return installed_;}
std::string FrameCapture::Detail() const {std::scoped_lock lock(mutex_);return detail_;}

FrameCapture::Admission FrameCapture::Request(Options options) {
    std::scoped_lock lock(mutex_);
    if (!installed_) return {0,"unavailable","Native frame capture is unavailable: "+detail_};
    for (auto& [id,entry]:entries_) {
        // Expire a stale request first so a caller that stopped polling cannot block captures forever.
        const auto age=now_()-entry.since;
        if ((entry.phase==Phase::Waiting&&age>=limits_.firstFrame)||(entry.phase==Phase::Copying&&age>=limits_.readback))
            Finish(entry,State::Failed,"frames_not_presented",NotPresentingMessage(entry,now_()));
        if (entry.phase==Phase::Waiting||entry.phase==Phase::Copying)
            return {0,"capture_busy","Another native capture is waiting for a frame"};
    }
    if (encoding_>=kMaxEncodeBacklog) return {0,"capture_busy","Earlier captures are still being encoded"};
    const auto id=nextId_++;
    Entry entry;
    entry.since=now_();
    entry.presentsAtRequest=Presents();
    entry.options=std::move(options);
    entry.status.state=State::Pending;
    entries_[id]=std::move(entry);
    waiting_.store(true,std::memory_order_release);
    Retire();
    return {id,{},{}};
}

std::string FrameCapture::NotPresentingMessage(const Entry& entry,RequestTime now) const {
    const auto presents=Presents();
    std::string last="none since the hook was installed";
    if (const auto ticks=lastPresentTicks_.load(std::memory_order_relaxed);presents&&ticks)
        last="last one "+std::to_string(Milliseconds(now-RequestTime(RequestTime::duration(ticks))))+" ms ago";
    const auto what=entry.phase==Phase::Copying?"the frame was copied but no later Present arrived to read it back":
        "no Present reached the game's swap chain";
    return "Fallout 4 is not presenting frames: "+std::string(what)+" within "+std::to_string(Milliseconds(now-entry.since))+
        " ms of the request (presents counted: "+std::to_string(presents)+", "+last+
        "). Typical causes: minimized window, display off or disconnected, a locked session, or a stalled render thread.";
}

FrameCapture::Status FrameCapture::Poll(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto it=entries_.find(id);
    if (it==entries_.end()) {Status status;status.code="not_found";status.message="No native capture has this id";return status;}
    auto& entry=it->second;
    const auto now=now_();
    const auto age=now-entry.since;
    if ((entry.phase==Phase::Waiting&&age>=limits_.firstFrame)||(entry.phase==Phase::Copying&&age>=limits_.readback))
        Finish(entry,State::Failed,"frames_not_presented",NotPresentingMessage(entry,now));
    else if (entry.phase==Phase::Encoding&&age>=limits_.encode)
        Finish(entry,State::Failed,"capture_failed","PNG encoding did not finish within "+std::to_string(limits_.encode.count())+" ms");
    auto status=entry.status;
    if (entry.phase!=Phase::Done) status.presentCount=Presents();
    return status;
}

void FrameCapture::Cancel(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    if (const auto it=entries_.find(id);it!=entries_.end()&&it->second.phase!=Phase::Done)
        Finish(it->second,State::Failed,"cancelled","Native capture was cancelled");
}

void FrameCapture::Finish(Entry& entry,State state,std::string code,std::string message) {
    entry.phase=Phase::Done;
    entry.status.state=state;
    entry.status.code=std::move(code);
    entry.status.message=std::move(message);
    entry.status.presentCount=Presents();
    waiting_.store(std::any_of(entries_.begin(),entries_.end(),[](const auto& e){return e.second.phase==Phase::Waiting;}),
        std::memory_order_release);
}

void FrameCapture::Retire() {
    for (auto it=entries_.begin();entries_.size()>kRetainedResults&&it!=entries_.end();)
        it=it->second.phase==Phase::Done?entries_.erase(it):std::next(it);
}

std::optional<std::uint64_t> FrameCapture::OnPresent() noexcept {
    try {
        presents_.fetch_add(1,std::memory_order_relaxed);
        const auto now=now_();
        lastPresentTicks_.store(now.time_since_epoch().count(),std::memory_order_relaxed);
        {
            std::scoped_lock history(historyMutex_);
            if (history_.size()<kPresentHistory) history_.push_back(now);
            else history_[historyNext_]=now;
            historyNext_=(historyNext_+1)%kPresentHistory;
        }
        if (!waiting_.load(std::memory_order_acquire)) return std::nullopt;
        std::scoped_lock lock(mutex_);
        for (auto& [id,entry]:entries_) {
            if (entry.phase!=Phase::Waiting) continue;
            entry.phase=Phase::Copying;
            entry.since=now;
            waiting_.store(false,std::memory_order_release);
            return id;
        }
        waiting_.store(false,std::memory_order_release);
    } catch (...) {}
    return std::nullopt;
}

void FrameCapture::Deliver(std::uint64_t id,RawFrame frame) {
    std::scoped_lock lock(mutex_);
    const auto it=entries_.find(id);
    if (it==entries_.end()||it->second.phase!=Phase::Copying) return;
    auto& entry=it->second;
    entry.status.dxgiFormat=frame.dxgiFormat;
    if (!OutputBytesPerPixel(frame.layout)) {
        Finish(entry,State::Failed,"unsupported_format","Swap chain format "+std::to_string(frame.dxgiFormat)+" cannot be saved as PNG");
        return;
    }
    entry.phase=Phase::Encoding;
    entry.since=now_();
    entry.status.name=FileName(id,entry.options.label);
    entry.status.path=(directory_/entry.status.name).string();
    jobs_.push_back({id,std::move(frame),directory_/entry.status.name,entry.options.stats});
    ++encoding_;
    wake_.notify_one();
}

void FrameCapture::Fail(std::uint64_t id,std::string code,std::string message) {
    std::scoped_lock lock(mutex_);
    if (const auto it=entries_.find(id);it!=entries_.end()&&(it->second.phase==Phase::Waiting||it->second.phase==Phase::Copying))
        Finish(it->second,State::Failed,std::move(code),std::move(message));
}

void FrameCapture::Work(std::stop_token stop) {
    for (;;) {
        Job job;
        {
            std::unique_lock lock(mutex_);
            if (!wake_.wait(lock,stop,[this]{return !jobs_.empty();})) break;
            job=std::move(jobs_.front());
            jobs_.pop_front();
        }
        std::string error;
        std::error_code ignored;
        std::optional<FrameStats> stats;
        if (job.stats) stats=ComputeFrameStats(job.frame,*job.stats);
        std::filesystem::create_directories(job.path.parent_path(),ignored);
        const bool written=WriteFramePng(job.frame,job.path,error);
        auto info=written?InspectPng(job.path.string()):ArtifactInfo{};
        if (!written) info.error=error;
        std::scoped_lock lock(mutex_);
        --encoding_;
        const auto it=entries_.find(job.id);
        if (it==entries_.end()||it->second.phase!=Phase::Encoding) continue;
        it->second.status.artifact=info;
        it->second.status.stats=std::move(stats);
        if (info.ok) Finish(it->second,State::Completed,{},{});
        else Finish(it->second,State::Failed,"capture_failed",info.error);
    }
}

std::vector<FileEntry> FrameCapture::Files() const {
    std::vector<FileEntry> files;
    std::error_code error;
    for (std::filesystem::directory_iterator it(directory_,error),end;!error&&it!=end;it.increment(error)) {
        const auto wide=it->path().filename().wstring();
        if (!std::all_of(wide.begin(),wide.end(),[](wchar_t c){return c>=32&&c<=126;})) continue;
        std::string name;
        for (const wchar_t c:wide) name+=static_cast<char>(c);
        if (!name.ends_with(".png")||!it->is_regular_file(error)) continue;
        const auto bytes=it->file_size(error);
        const auto modified=it->last_write_time(error).time_since_epoch().count();
        if (error) {error.clear();continue;}
        files.push_back({name,it->path().string(),bytes,modified});
        if (files.size()>=4096) break;
    }
    return files;
}
std::vector<RequestTime> FrameCapture::PresentsSince(RequestTime since) const {
    std::scoped_lock lock(historyMutex_);
    std::vector<RequestTime> times;
    const auto start=history_.size()<kPresentHistory?0:historyNext_;
    for (std::size_t i=0;i<history_.size();++i) {
        const auto& t=history_[(start+i)%history_.size()];
        if (t>=since) times.push_back(t);
    }
    return times;
}
}
