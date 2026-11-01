#include "core/LogTools.h"
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>

namespace fusionmcp {
namespace {
char Fold(char c) {return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));}
std::uint64_t FileSize(const std::string& path) {
    std::error_code error;
    const auto size=std::filesystem::file_size(std::filesystem::path(path),error);
    return error?0:size;
}
}

bool WildcardMatch(std::string_view pattern,std::string_view text) {
    std::size_t p=0,t=0,star=std::string_view::npos,mark=0;
    while (t<text.size()) {
        if (p<pattern.size()&&pattern[p]=='*') {star=p++;mark=t;}
        else if (p<pattern.size()&&Fold(pattern[p])==Fold(text[t])) {++p;++t;}
        else if (star!=std::string_view::npos) {p=star+1;t=++mark;}
        else return false;
    }
    while (p<pattern.size()&&pattern[p]=='*') ++p;
    return p==pattern.size();
}

std::optional<FileEntry> ResolveLog(const std::vector<FileEntry>& logs,const std::string& wanted) {
    const FileEntry* best=nullptr;
    const bool pattern=wanted.find('*')!=std::string::npos;
    for (const auto& f:logs) {
        if (!WildcardMatch(wanted,f.name)) continue;
        if (!pattern) return f;
        if (!best||f.modified>best->modified) best=&f;
    }
    return best?std::optional<FileEntry>(*best):std::nullopt;
}

std::string FormatMarker(const LogMarker& m) {return m.log+"#"+std::to_string(m.offset);}
std::optional<LogMarker> ParseMarker(const std::string& text) {
    const auto hash=text.rfind('#');
    if (hash==std::string::npos||hash==0||hash+1>=text.size()||text.size()-hash>21) return std::nullopt;
    const auto digits=std::string_view(text).substr(hash+1);
    if (!std::all_of(digits.begin(),digits.end(),[](unsigned char c){return std::isdigit(c);})) return std::nullopt;
    return LogMarker{text.substr(0,hash),std::stoull(std::string(digits))};
}

void LogMarkers::Store(const std::string& id,const LogMarker& marker) {
    std::scoped_lock lock(mutex_);
    stored_[id]={++sequence_,marker};
    while (stored_.size()>kMaxStored) {
        const auto oldest=std::min_element(stored_.begin(),stored_.end(),[](const auto& a,const auto& b){return a.second.first<b.second.first;});
        stored_.erase(oldest);
    }
}
std::optional<LogMarker> LogMarkers::Find(const std::string& key) const {
    {
        std::scoped_lock lock(mutex_);
        if (const auto it=stored_.find(key);it!=stored_.end()) return it->second.second;
    }
    return ParseMarker(key);
}

LogWaiter::LogWaiter(Lister list,std::string log,std::regex pattern,std::optional<std::uint64_t> from,
        std::chrono::milliseconds timeout,std::chrono::milliseconds interval)
    :list_(std::move(list)),log_(std::move(log)),pattern_(std::move(pattern)),from_(from),timeout_(timeout),interval_(interval),
     worker_([this](std::stop_token stop){Run(stop);}) {}
LogWaiter::~LogWaiter() {Stop();}
void LogWaiter::Stop() {
    worker_.request_stop();
    if (worker_.joinable()&&worker_.get_id()!=std::this_thread::get_id()) worker_.join();
}
LogWaiter::Result LogWaiter::Poll() const {std::scoped_lock lock(mutex_);return result_;}

void LogWaiter::Run(std::stop_token stop) {
    const auto deadline=std::chrono::steady_clock::now()+timeout_;
    const auto finish=[&](Result::State state,std::string error={}) {
        std::scoped_lock lock(mutex_);
        result_.state=state;result_.error=std::move(error);
    };
    std::optional<FileEntry> file;
    try {file=ResolveLog(list_(),log_);} catch (...) {}
    if (!file) {finish(Result::State::Failed,"No listed log matches "+log_);return;}
    std::uint64_t offset=from_.value_or(FileSize(file->path));
    {std::scoped_lock lock(mutex_);result_.log=file->name;result_.startOffset=result_.nextOffset=offset;}
    std::string partial;
    std::uint64_t partialOffset=offset;
    while (!stop.stop_requested()) {
        const auto size=FileSize(file->path);
        if (size<offset) {
            offset=0;partial.clear();partialOffset=0;
            std::scoped_lock lock(mutex_);result_.rotated=true;
        }
        if (size>offset) {
            std::ifstream in(std::filesystem::path(file->path),std::ios::binary);
            const auto count=std::min<std::uint64_t>(size-offset,1<<20);
            std::string chunk(static_cast<std::size_t>(count),'\0');
            in.seekg(static_cast<std::streamoff>(offset));
            in.read(chunk.data(),static_cast<std::streamsize>(count));
            chunk.resize(static_cast<std::size_t>(in.gcount()));
            offset+=chunk.size();
            partial+=chunk;
            std::size_t start=0;
            for (auto end=partial.find('\n');end!=std::string::npos;start=end+1,end=partial.find('\n',start)) {
                auto line=partial.substr(start,end-start);
                if (!line.empty()&&line.back()=='\r') line.pop_back();
                if (line.size()>kMaxLineBytes) line.resize(kMaxLineBytes);
                if (std::regex_search(line,pattern_)) {
                    std::scoped_lock lock(mutex_);
                    result_.state=Result::State::Matched;result_.line=std::move(line);
                    result_.lineOffset=partialOffset+start;result_.nextOffset=partialOffset+end+1;
                    result_.scanned=result_.nextOffset-result_.startOffset;
                    return;
                }
            }
            partialOffset+=start;partial.erase(0,start);
            // Bounded memory: an unterminated run longer than a line is dropped.
            if (partial.size()>kMaxLineBytes) {partialOffset+=partial.size();partial.clear();}
            std::scoped_lock lock(mutex_);
            result_.nextOffset=partialOffset;result_.scanned=offset-result_.startOffset;
        }
        if (std::chrono::steady_clock::now()>=deadline) {finish(Result::State::TimedOut);return;}
        std::this_thread::sleep_for(interval_);
    }
}
}
