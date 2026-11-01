#include "BatchCoordinator.h"
#include <algorithm>

namespace fusionmcp {
namespace {
constexpr const char* kWaitReady="fusionmcp.wait_ready";
constexpr const char* kLoad="fusionmcp.saves.load";
bool Terminal(const std::string& state) {
    return state=="completed"||state=="failed"||state=="cancelled"||state=="interrupted";
}
std::int64_t Ms(RequestTime from,RequestTime to) {
    return to<from?0:std::chrono::duration_cast<std::chrono::milliseconds>(to-from).count();
}
Json Bounded(Json result,std::size_t limit) {
    const auto bytes=result.dump().size();
    if (bytes<=limit) return result;
    return {{"truncated",true},{"bytes",bytes},{"ok",result.value("ok",false)}};
}
}

BatchCoordinator::BatchCoordinator(ProviderRegistry& core,RequestBroker::NowFn now):core_(core),now_(std::move(now)) {}

std::vector<ProviderRegistry::ControlTool> BatchCoordinator::ControlTools() {
    const auto idSchema=ParseBounded(R"({"type":"object","properties":{"batchId":{"type":"integer","minimum":1}},"required":["batchId"],"additionalProperties":false})");
    return {
        {"fusionmcp.batch.execute","Run up to 32 tool calls in order. Each step starts only after the previous one reached an actual terminal "
            "state; delayAfterMs is a fixed delay after completion. A fusionmcp.saves.load step must be followed by fusionmcp.wait_ready, which "
            "receives the load's transactionId. Stops on the first failure unless continueOnFailure. Poll fusionmcp.batch.get.",
            ParseBounded(R"({"type":"object","properties":{
"steps":{"type":"array","minItems":1,"maxItems":32,"items":{"type":"object","properties":{
"tool":{"type":"string","minLength":1,"maxLength":128},
"arguments":{"type":"object","additionalProperties":true},
"delayAfterMs":{"type":"integer","minimum":0,"maximum":60000},
"timeoutMs":{"type":"integer","minimum":1,"maximum":180000}},"required":["tool"],"additionalProperties":false}},
"continueOnFailure":{"type":"boolean"}},"required":["steps"],"additionalProperties":false})"),true,
            [this](const Json& args,bool allowMutations){return Execute(args,allowMutations);},"ordered_batches"},
        {"fusionmcp.batch.get","Per-step state, request ids, epochs, timing and bounded results of a batch.",idSchema,false,
            [this](const Json& args,bool){return Get(args["batchId"].get<std::uint64_t>());}},
        {"fusionmcp.batch.cancel","Stop a batch: no later step starts and the active request is cancelled. Started effects are not undone.",
            idSchema,true,[this](const Json& args,bool){return Cancel(args["batchId"].get<std::uint64_t>());}},
    };
}
bool BatchCoordinator::RegisterWith(ProviderRegistry& core) {
    for (auto& tool:ControlTools()) if (!core.RegisterControl(std::move(tool))) return false;
    return true;
}
Json BatchCoordinator::Execute(const Json& args,bool allowMutations) {
    Batch b;
    b.allowMutations=allowMutations;b.continueOnFailure=args.value("continueOnFailure",false);
    const auto& steps=args["steps"];
    for (std::size_t i=0;i<steps.size();++i) {
        const auto& item=steps[i];
        const auto where=" (step "+std::to_string(i)+")";
        Step s;
        s.tool=item["tool"].get<std::string>();
        s.arguments=item.value("arguments",Json::object());
        s.delayMs=item.value("delayAfterMs",0ull);
        s.timeoutMs=item.value("timeoutMs",static_cast<std::uint64_t>(s.tool==kWaitReady?180000:
            std::chrono::duration_cast<std::chrono::milliseconds>(kDefaultStepTimeout).count()));
        if (s.tool.starts_with("fusionmcp.batch.")) return Error("invalid_arguments","Batches cannot be nested"+where);
        const auto descriptor=core_.Lookup(s.tool);
        if (descriptor.is_null()) return Error("unsupported","Tool is not registered"+where);
        if (descriptor["_meta"]["fusionmcp.mutating"].get<bool>()&&!allowMutations) return Error("read_only","Mutations disabled"+where);
        if (!MatchesSchema(s.arguments,descriptor["inputSchema"])) return Error("invalid_arguments","Arguments do not match the tool schema"+where);
        if (s.tool==kLoad&&(i+1>=steps.size()||steps[i+1]["tool"]!=kWaitReady))
            return Error("invalid_arguments","fusionmcp.saves.load must be followed by fusionmcp.wait_ready"+where);
        b.steps.push_back(std::move(s));
    }
    std::scoped_lock lock(mutex_);
    if (std::any_of(batches_.begin(),batches_.end(),[](const Batch& x){return x.state=="running";}))
        return Error("busy","Another batch is running");
    if (batches_.size()>=kMaxRetained) batches_.erase(batches_.begin());
    b.id=next_++;b.baseline=core_.Epoch();b.begun=now_();
    batches_.push_back(std::move(b));
    auto& stored=batches_.back();
    Advance(stored);
    return Success({{"batchId",stored.id},{"state",stored.state},{"completed",stored.state!="running"},
        {"steps",stored.steps.size()}});
}
void BatchCoordinator::Complete(Batch& b,Step& s,std::string state,Json result) {
    s.state=std::move(state);s.result=Bounded(std::move(result),kMaxStepResultBytes);
    s.ended=now_();s.epochEnd=core_.Epoch();s.polling=false;
    const bool ok=s.state=="completed";
    if (ok&&s.tool==kLoad) {
        b.transition=true;
        // The load is a Game owner request: its envelope is the polled outcome.
        const auto& envelope=s.result.contains("outcome")?s.result["outcome"]:s.result;
        const auto transaction=envelope.value("result",Json::object()).value("transactionId",0ull);
        if (transaction&&b.current+1<b.steps.size()&&!b.steps[b.current+1].arguments.contains("transactionId"))
            b.steps[b.current+1].arguments["transactionId"]=transaction;
    }
    if (ok&&s.tool==kWaitReady) {b.transition=false;b.baseline=core_.Epoch();}
    if (!ok&&!b.continueOnFailure) {b.state="failed";b.reason="Step "+std::to_string(b.current)+" "+s.state;return;}
    if (s.delayMs) {s.delaying=true;s.resumeAt=s.ended+std::chrono::milliseconds(s.delayMs);}
    else ++b.current;
}
void BatchCoordinator::Advance(Batch& b) {
    while (b.state=="running") {
        if (now_()-b.begun>=kOverallTimeout) {
            if (b.current<b.steps.size()&&b.steps[b.current].polling) core_.Cancel(b.steps[b.current].requestId);
            b.state="failed";b.reason="Batch deadline expired; started steps are not undone";return;
        }
        if (b.current>=b.steps.size()) {
            const bool failures=std::any_of(b.steps.begin(),b.steps.end(),[](const Step& s){return s.state!="completed";});
            b.state=failures?"completed_with_failures":"completed";return;
        }
        auto& s=b.steps[b.current];
        if (s.delaying) {
            if (now_()<s.resumeAt) return;
            s.delaying=false;++b.current;continue;
        }
        if (s.state=="not_run") {
            if (!b.transition&&s.tool!=kWaitReady&&core_.Epoch()!=b.baseline) {
                b.state="interrupted";b.reason="Load epoch changed outside a batch-owned save load";return;
            }
            s.state="running";s.started=now_();s.epochStart=core_.Epoch();
            const auto r=core_.Invoke(s.tool,s.arguments.dump(),b.allowMutations);
            if (!r.value("ok",false)) {Complete(b,s,"failed",r);continue;}
            const auto& result=r["result"];
            if (s.tool!=kWaitReady&&result.is_object()&&result.contains("requestId")&&result.value("completed",true)==false) {
                s.requestId=result["requestId"].get<std::uint64_t>();s.polling=true;return;
            }
            if (s.tool!=kWaitReady) {Complete(b,s,"completed",r);continue;}
        }
        const bool expired=Ms(s.started,now_())>=static_cast<std::int64_t>(s.timeoutMs);
        if (s.tool==kWaitReady) {
            const auto r=core_.Invoke(s.tool,s.arguments.dump(),b.allowMutations);
            const auto& result=r.value("result",Json::object());
            if (!r.value("ok",false)) {Complete(b,s,"failed",r);continue;}
            if (result.value("ready",false)) {Complete(b,s,"completed",r);continue;}
            if (result.value("terminal",false)) {Complete(b,s,"failed",r);continue;}
            if (expired) {Complete(b,s,"failed",Error("timeout","World was not ready before the step deadline; the load may still finish"));continue;}
            return;
        }
        const auto q=core_.Query(s.requestId);
        const auto state=q.value("result",Json::object()).value("state",std::string());
        if (Terminal(state)) {
            const bool ok=state=="completed"&&q["result"]["outcome"].value("ok",false);
            Complete(b,s,ok?"completed":"failed",q["result"]);continue;
        }
        if (expired) {
            core_.Cancel(s.requestId);
            Complete(b,s,"failed",Error("timeout","Step deadline expired; a started effect may still have happened"));continue;
        }
        return;
    }
}
Json BatchCoordinator::Describe(const Batch& b) const {
    Json steps=Json::array();
    for (std::size_t i=0;i<b.steps.size();++i) {
        const auto& s=b.steps[i];
        Json step={{"index",i},{"tool",s.tool},{"state",s.state},{"epochStart",s.epochStart},{"epochEnd",s.epochEnd}};
        if (s.requestId) step["requestId"]=s.requestId;
        if (s.state!="not_run") step["startedMs"]=Ms(b.begun,s.started);
        if (!s.result.is_null()) {step["endedMs"]=Ms(b.begun,s.ended);step["result"]=s.result;}
        if (s.delaying) step["delayRemainingMs"]=Ms(now_(),s.resumeAt);
        steps.push_back(std::move(step));
    }
    return {{"batchId",b.id},{"state",b.state},{"reason",b.reason},{"currentStep",b.current},
        {"elapsedMs",Ms(b.begun,now_())},{"steps",std::move(steps)}};
}
Json BatchCoordinator::Get(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto it=std::find_if(batches_.begin(),batches_.end(),[&](const Batch& b){return b.id==id;});
    if (it==batches_.end()) return Error("not_found","Batch not retained");
    Advance(*it);
    return Success(Describe(*it));
}
Json BatchCoordinator::Cancel(std::uint64_t id) {
    std::scoped_lock lock(mutex_);
    const auto it=std::find_if(batches_.begin(),batches_.end(),[&](const Batch& b){return b.id==id;});
    if (it==batches_.end()) return Error("not_found","Batch not retained");
    auto& b=*it;
    if (b.state=="running") {
        if (b.current<b.steps.size()) {
            auto& s=b.steps[b.current];
            if (s.polling) core_.Cancel(s.requestId);
            if (s.state=="running") {s.state="cancelled";s.ended=now_();s.polling=false;}
        }
        b.state="cancelled";b.reason="Cancelled; started steps are not undone";
    }
    return Success(Describe(b));
}
void BatchCoordinator::Tick() {
    std::scoped_lock lock(mutex_);
    for (auto& b:batches_) if (b.state=="running") Advance(b);
}
bool BatchCoordinator::Running() const {
    std::scoped_lock lock(mutex_);
    return std::any_of(batches_.begin(),batches_.end(),[](const Batch& b){return b.state=="running";});
}
}
