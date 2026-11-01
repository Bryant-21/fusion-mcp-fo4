#include "SaveLoadCoordinator.h"
#include <algorithm>

namespace fusionmcp {
namespace {
std::string Fold(std::string_view text) {
    std::string folded(text);
    for (char& c:folded) if (c>='A'&&c<='Z') c=static_cast<char>(c+('a'-'A'));
    return folded;
}
std::string Stem(std::string_view path) {
    const auto slash=path.find_last_of("\\/");
    if (slash!=path.npos) path.remove_prefix(slash+1);
    if (path.size()>4&&Fold(path.substr(path.size()-4))==".fos") path.remove_suffix(4);
    return std::string(path);
}
}

SaveLoadCoordinator::SaveLoadCoordinator(RequestBroker::NowFn now):now_(std::move(now)) {}
SaveLoadCoordinator::Transaction* SaveLoadCoordinator::Find(std::uint64_t id) {
    if (history_.empty()) return nullptr;
    if (!id) return &history_.back();
    const auto it=std::find_if(history_.begin(),history_.end(),[&](const auto& t){return t.id==id;});
    return it==history_.end()?nullptr:&*it;
}
const SaveLoadCoordinator::Transaction* SaveLoadCoordinator::Find(std::uint64_t id) const {
    return const_cast<SaveLoadCoordinator*>(this)->Find(id);
}
SaveLoadCoordinator::Transaction* SaveLoadCoordinator::Active() {
    for (auto& t:history_) Expire(t);
    return !history_.empty()&&!history_.back().terminal?&history_.back():nullptr;
}
void SaveLoadCoordinator::Expire(Transaction& t) const {
    if (!t.terminal&&now_()-t.begun>=kTimeout) {
        t.terminal=true;t.phase="timeout";t.reason="No correlated ready world before the deadline; the engine load may still have happened";
    }
}
void SaveLoadCoordinator::Finish(Transaction& t,std::string phase,std::string reason) {
    if (t.terminal) return;
    t.terminal=true;t.phase=std::move(phase);t.reason=std::move(reason);
}
Json SaveLoadCoordinator::Describe(const Transaction& t) const {
    return {{"transactionId",t.id},{"save",t.save},{"phase",t.phase},{"ready",t.phase=="ready"},{"terminal",t.terminal},
        {"reason",t.reason},{"startEpoch",t.startEpoch},{"resultEpoch",t.resultEpoch},{"readySamples",t.readySamples},
        {"elapsedMs",std::chrono::duration_cast<std::chrono::milliseconds>(now_()-t.begun).count()}};
}
Json SaveLoadCoordinator::Validate(const std::string& save,const std::vector<FileEntry>& saves) const {
    std::scoped_lock lock(mutex_);
    if (!history_.empty()&&!history_.back().terminal&&now_()-history_.back().begun<kTimeout)
        return Error("load_in_progress","Another save load transaction is active");
    const auto wanted=Fold(save);
    std::vector<const FileEntry*> matches;
    for (const auto& entry:saves) if (Fold(Stem(entry.name))==wanted&&Fold(entry.name).ends_with(".fos")) matches.push_back(&entry);
    if (matches.empty()) return Error("save_not_found","No save file has exactly this name");
    if (matches.size()>1) return Error("ambiguous_save",std::to_string(matches.size())+" save files match this name; refusing to pick one");
    return Success({{"save",Stem(matches[0]->name)},{"bytes",matches[0]->bytes}});
}
std::uint64_t SaveLoadCoordinator::Begin(const std::string& save,EpochId epoch) {
    std::scoped_lock lock(mutex_);
    if (history_.size()>=16) history_.erase(history_.begin());
    Transaction t;t.id=next_++;t.save=save;t.startEpoch=epoch;t.begun=now_();
    history_.push_back(std::move(t));
    return history_.back().id;
}
void SaveLoadCoordinator::Dispatched(std::uint64_t id,bool accepted) {
    std::scoped_lock lock(mutex_);
    auto* t=Find(id);
    if (!t||t->terminal) return;
    if (accepted) t->phase="dispatched"; else Finish(*t,"failed","Native console refused the load line");
}
void SaveLoadCoordinator::OnPreLoad(std::string_view path) {
    std::scoped_lock lock(mutex_);
    auto* t=Active();
    if (!t) return;
    const auto stem=Stem(path);
    if (stem.empty()) {Finish(*t,"uncorrelated","PreLoadGame carried no save path; target identity cannot be proven");return;}
    if (t->phase=="dispatched"&&Fold(stem)==Fold(t->save)) {t->phase="loading";return;}
    Finish(*t,"superseded","Unrelated load of "+stem);
}
void SaveLoadCoordinator::OnPostLoad(bool success,EpochId resultEpoch) {
    std::scoped_lock lock(mutex_);
    auto* t=Active();
    if (!t) return;
    if (t->phase!="loading") {Finish(*t,"uncorrelated","PostLoadGame without a matching PreLoadGame");return;}
    // Observed on 1.11.240: a save needing missing Creations reports failure
    // while the engine shows its download/continue prompts.
    if (!success) {Finish(*t,"failed","Engine reported the load did not complete; it may be waiting on a missing-content prompt");return;}
    t->phase="loaded";t->resultEpoch=resultEpoch;
}
void SaveLoadCoordinator::OnNewGame() {
    std::scoped_lock lock(mutex_);
    if (auto* t=Active()) Finish(*t,"superseded","A new game started");
}
void SaveLoadCoordinator::Sample(const ReadinessSample& s,EpochId epoch) {
    std::scoped_lock lock(mutex_);
    auto* t=Active();
    if (!t||t->phase!="loaded"||epoch!=t->resultEpoch) return;
    ++t->samples;
    const bool ready=s.sampled&&!s.loadingMenuOpen&&!s.mainMenuOpen&&s.playerPresent&&s.parentCellAttached&&s.player3DLoaded;
    if (!ready) {t->readySamples=0;return;}
    if (!t->readySamples++) t->firstReady=now_();
    if (t->readySamples>=kStableSamples&&now_()-t->firstReady>=kStableSpan)
        Finish(*t,"ready","Target save loaded successfully and the world was stably ready on the Game owner");
}
bool SaveLoadCoordinator::NeedsSamples() const {
    std::scoped_lock lock(mutex_);
    return !history_.empty()&&!history_.back().terminal&&history_.back().phase=="loaded";
}
Json SaveLoadCoordinator::Snapshot(std::uint64_t id) const {
    std::scoped_lock lock(mutex_);
    auto* self=const_cast<SaveLoadCoordinator*>(this);
    for (auto& t:self->history_) Expire(t);
    const auto* t=Find(id);
    return t?Describe(*t):Json();
}
Json SaveLoadCoordinator::Cancel(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    auto* t=Find(id);
    if (!t) return Json();
    Expire(*t);
    Finish(*t,"cancelled","Stopped waiting; cancellation cannot undo an engine load that already started");
    return Describe(*t);
}
}
