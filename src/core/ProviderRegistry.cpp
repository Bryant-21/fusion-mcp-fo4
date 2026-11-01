#include "ProviderRegistry.h"
#include "Limits.h"
#include <algorithm>
#include <array>
#include <map>
#include <mutex>
#include <limits>
#include <deque>

namespace fusionmcp {
namespace {
bool Identifier(std::string_view text, bool dots=false) {
    if (text.empty() || text.size()>96 || text.front()=='.' || text.back()=='.') return false;
    for (const char c:text) if (!((c>='a'&&c<='z')||(c>='0'&&c<='9')||c=='_'||(dots&&c=='.'))) return false;
    return text.find("..") == text.npos;
}
Owner ToOwner(FUSIONMCP_Owner owner) {
    switch(owner) {
    case FUSIONMCP_OWNER_GAME:return Owner::Game;
    case FUSIONMCP_OWNER_RENDER:return Owner::Render;
    case FUSIONMCP_OWNER_UI:return Owner::UI;
    case FUSIONMCP_OWNER_VM:return Owner::VM;
    default:throw std::invalid_argument("owner");
    }
}
// 30 s, extended by a command's own timeoutMs or durationMs argument (up to 10 min)
// so long waits, samples and camera paths are not cut off by the default.
std::chrono::milliseconds RequestDeadline(const Json& args) {
    std::int64_t extra=0;
    for (const char* key:{"timeoutMs","durationMs"})
        if (args.is_object()&&args.contains(key)&&args[key].is_number_integer()) extra=std::max<std::int64_t>(extra,args[key].get<std::int64_t>());
    return std::chrono::milliseconds(30000+std::clamp<std::int64_t>(extra,0,600000));
}
const char* OwnerName(Owner owner) {
    switch(owner) {case Owner::Game:return "game";case Owner::Render:return "render";case Owner::UI:return "ui";case Owner::VM:return "vm";}
    return "unavailable";
}
struct Sink {
    std::string bytes;
    unsigned calls=0;
    bool failed=false;
    static uint32_t FUSIONMCP_CALL Write(void* opaque,const char* data,uint32_t size) noexcept {
        auto& s=*static_cast<Sink*>(opaque);
        try {
            if (++s.calls!=1 || !data || size>FUSIONMCP_MAX_RESULT_BYTES) {s.failed=true;return 0;}
            s.bytes.assign(data,size);return 1;
        } catch (...) {s.failed=true;return 0;}
    }
};
Json Descriptor(std::string name,std::string description,Json schema,bool mutation=false) {
    return {{"name",std::move(name)},{"description",std::move(description)}, {"inputSchema",std::move(schema)},
        {"annotations",{{"readOnlyHint",!mutation}}},{"_meta",{{"fusionmcp.mutating",mutation},{"fusionmcp.owner","control"},{"fusionmcp.available",true}}}};
}
std::string Namespace(std::string_view name) {return std::string(name.substr(0,name.find('.')));}
std::string UnavailableReason(const ProviderRegistry::ControlTool& c) {
    if (!c.unavailableReason) return {};
    try {return c.unavailableReason();} catch (...) {return "Availability check failed";}
}
bool Mutates(const ProviderRegistry::ControlTool& c,const Json& args) {
    if (c.mutating) return true;
    const auto action=args.is_object()?args.find("action"):args.end();
    return action!=args.end()&&action->is_string()&&
        std::find(c.mutatingActions.begin(),c.mutatingActions.end(),action->get<std::string>())!=c.mutatingActions.end();
}
Json DescribeControl(const ProviderRegistry::ControlTool& c) {
    auto tool=Descriptor(c.name,c.description,c.schema,c.mutating);
    const auto reason=UnavailableReason(c);
    tool["_meta"]["fusionmcp.available"]=reason.empty();
    if (!reason.empty()) tool["_meta"]["fusionmcp.unavailableReason"]=reason;
    if (!c.mutatingActions.empty()) {
        tool["annotations"]["readOnlyHint"]=false;
        tool["_meta"]["fusionmcp.mutatingActions"]=c.mutatingActions;
    }
    return tool;
}
Json EmptySchema() { return {{"type","object"},{"properties",Json::object()},{"additionalProperties",false}}; }
Json IdSchema() { return {{"type","object"},{"properties",{{"requestId",{{"type","integer"},{"minimum",1}}}}},
    {"required",Json::array({"requestId"})},{"additionalProperties",false}}; }
}

struct ProviderRegistry::State : std::enable_shared_from_this<State> {
    struct Command {std::string name,description;Json schema;Owner owner;uint32_t flags;uint64_t capabilities;};
    struct Entry {
        uint64_t handle=0,generation=1,capabilities=0;
        bool enabled=false,builtin=false;
        std::size_t leases=0;
        std::string nameSpace;
        std::vector<std::string> supported;
        std::vector<Command> commands;
        void* context=nullptr;
        FUSIONMCP_InvokeFn invoke=nullptr;
        FUSIONMCP_PollFn poll=nullptr;
        FUSIONMCP_CancelFn cancel=nullptr;
    };
    struct RequestData {
        RequestId id=0,job=0;
        EpochId epoch=0;
        uint64_t generation=0;
        std::shared_ptr<Entry> entry;
        Command command;
        std::string arguments,operation;
        RequestTime deadline,nextPoll;
        std::string phase="queued";
        Json outcome=Json();
        bool started=false,terminal=false,executing=false,jobQueued=false,leased=false,quarantined=false;
        bool invalidatedWhileRunning=false;
    };
    mutable std::mutex mutex;
    RequestBroker::NowFn now;
    RequestBroker broker;
    std::map<uint64_t,std::shared_ptr<Entry>> providers;
    std::map<uint64_t,std::shared_ptr<RequestData>> requests;
    std::vector<ControlTool> controls;
    std::array<bool,4> owners{};
    std::array<uint64_t,4> pollCursor{};
    uint64_t nextProvider=1,nextRequest=1;
    bool mutations=false,stopped=false;
    explicit State(RequestBroker::NowFn clock):now(std::move(clock)),broker(now) {}
    bool Available(Owner o) const {return owners.at(static_cast<std::size_t>(o));}
    Json DescribeCommand(const Entry& p,const Command& c,bool worstCase=false) const {
        auto tool=Descriptor(p.nameSpace+"."+c.name,c.description,c.schema,(c.flags&FUSIONMCP_COMMAND_MUTATING)!=0);
        tool["_meta"]={{"fusionmcp.mutating",(c.flags&FUSIONMCP_COMMAND_MUTATING)!=0},{"fusionmcp.owner",OwnerName(c.owner)},
            {"fusionmcp.available",!worstCase&&p.enabled&&Available(c.owner)},{"fusionmcp.capabilities",c.capabilities},{"fusionmcp.async",(c.flags&FUSIONMCP_COMMAND_ASYNC)!=0}};
        return tool;
    }
    Json Catalog(const Entry* candidate=nullptr,bool worstCase=false) const {
        Json tools=Json::array({Descriptor("fusionmcp.status","Standalone capabilities and lifecycle status",EmptySchema()),
            Descriptor("fusionmcp.request.get","Actual completion and retirement status",IdSchema()),
            Descriptor("fusionmcp.request.cancel","Request cancellation; cannot undo already-started effects",IdSchema(),true)});
        for (const auto& c:controls) tools.push_back(DescribeControl(c));
        for (const auto& [id,p]:providers) for (const auto& c:p->commands) tools.push_back(DescribeCommand(*p,c,worstCase));
        if (candidate) for (const auto& c:candidate->commands) tools.push_back(DescribeCommand(*candidate,c,worstCase));
        return tools;
    }
    void Terminal(RequestData& r,std::string phase,Json outcome) {
        if (r.terminal) return;
        r.terminal=true;r.phase=std::move(phase);r.outcome=std::move(outcome);
        r.invalidatedWhileRunning=r.executing;
        r.nextPoll=RequestTime{}; // Retirement is eligible immediately.
    }
    void Release(RequestData& r) {
        if (r.leased) {--r.entry->leases;r.leased=false;}
        r.operation.clear();
    }
    void Refresh(RequestData& r) {
        if (!r.terminal && now()>=r.deadline) Terminal(r,"failed",Error("timeout","Operation deadline expired; retirement may still be pending"));
        if (r.jobQueued && r.job && !r.executing) {
            const auto job=broker.Query(r.job);
            if (job && (job->state==RequestState::Cancelled||job->state==RequestState::Interrupted||job->state==RequestState::Failed)) {
                r.jobQueued=false;
                if (!r.leased && !r.terminal) Terminal(r,"interrupted",Error(job->errorCode,job->errorMessage));
            }
        }
    }
    uint32_t Remaining(const RequestData& r) const {
        const auto ms=std::chrono::duration_cast<std::chrono::milliseconds>(r.deadline-now()).count();
        return static_cast<uint32_t>(std::clamp<int64_t>(ms,0,60000));
    }
    Outcome Start(const std::shared_ptr<RequestData>& r) {
        {
            std::scoped_lock lock(mutex);r->jobQueued=false;Refresh(*r);
            if (r->terminal) return Outcome::Success();
            if (stopped || !r->entry->enabled || r->generation!=r->entry->generation || r->epoch!=broker.Epoch() ||
                !Available(r->command.owner) || ((r->command.flags&FUSIONMCP_COMMAND_MUTATING)&&!mutations)) {
                Terminal(*r,"interrupted",Error("authority_changed","Owner, provider, policy or epoch authority changed"));return Outcome::Success();
            }
            r->executing=true;r->started=true;r->phase="running";r->leased=true;++r->entry->leases;
        }
        Sink sink;
        const FUSIONMCP_InvocationV1 invocation{FUSIONMCP_ABI_V1,sizeof(FUSIONMCP_InvocationV1),r->id,r->epoch,Remaining(*r),0,
            r->command.name.data(),static_cast<uint32_t>(r->command.name.size()),r->arguments.data(),static_cast<uint32_t>(r->arguments.size())};
        const auto status=r->entry->invoke(r->entry->context,&invocation,&Sink::Write,&sink);
        std::scoped_lock lock(mutex);r->executing=false;
        if (status==FUSIONMCP_PENDING) {
            bool valid=(r->command.flags&FUSIONMCP_COMMAND_ASYNC)&&!sink.failed&&sink.calls==1&&
                !sink.bytes.empty()&&sink.bytes.size()<=128;
            if (valid) {
                // Validate UTF-8 without interpreting an opaque provider id.
                try {(void)Json(sink.bytes).dump();} catch (...) {valid=false;}
                for (const auto& [id,other]:requests) if (id!=r->id && other->leased && other->entry==r->entry && other->operation==sink.bytes) valid=false;
            }
            if (!valid) {
                // Provider admitted unknown resources. Never grant unregister OK
                // while we cannot prove their retirement; retain the lease.
                r->quarantined=true;r->entry->enabled=false;++r->entry->generation;
                Terminal(*r,"failed",Error("provider_contract","Invalid pending operation; provider lifetime quarantined"));
            } else {
                r->operation=std::move(sink.bytes);r->nextPoll=r->terminal?RequestTime{}:now()+std::chrono::milliseconds(10);
                if (!r->terminal) r->phase="pending";
                Refresh(*r);
            }
        } else {
            Final(*r,status,sink);Release(*r);
        }
        return Outcome::Success();
    }
    void Final(RequestData& r,FUSIONMCP_Status status,const Sink& sink) {
        if (r.terminal) return;
        if (status!=FUSIONMCP_OK || sink.failed || sink.calls!=1) {
            Terminal(r,"failed",Error("provider_failure","Provider did not return one bounded final result"));return;
        }
        try {
            auto result=ParseBounded(sink.bytes,FUSIONMCP_MAX_RESULT_BYTES,kMaxProviderResultDepth);
            if (!Envelope(result)) throw std::invalid_argument("envelope");
            // Canonical serialization can grow relative to the supplied JSON.
            // The stored envelope, not only callback input, must fit the budget.
            if (result.dump().size()>FUSIONMCP_MAX_RESULT_BYTES) throw std::invalid_argument("result byte limit");
            if (now()>=r.deadline) Terminal(r,"failed",Error("timeout","Operation completed after its deadline"));
            else {
                const bool ok=result["ok"].get<bool>();
                Terminal(r,ok?"completed":"failed",std::move(result));
            }
        } catch (...) {Terminal(r,"failed",Error("provider_failure","Invalid final result envelope"));}
    }
    Outcome Continue(const std::shared_ptr<RequestData>& r) {
        bool retiring;
        {
            std::scoped_lock lock(mutex);r->jobQueued=false;Refresh(*r);
            if (!r->leased || r->quarantined) return Outcome::Success();
            if (!Available(r->command.owner)) return Outcome::Success();
            if (!r->entry->enabled || r->generation!=r->entry->generation || r->epoch!=broker.Epoch() || stopped)
                Terminal(*r,"interrupted",Error("authority_changed","Provider or load epoch changed"));
            if ((r->command.flags&FUSIONMCP_COMMAND_MUTATING)&&!mutations)
                Terminal(*r,"cancelled",Error("read_only","Mutation authorization revoked; retiring operation"));
            retiring=r->terminal;r->executing=true;
        }
        const FUSIONMCP_OperationV1 op{FUSIONMCP_ABI_V1,sizeof(FUSIONMCP_OperationV1),r->id,r->epoch,Remaining(*r),0,
            r->operation.data(),static_cast<uint32_t>(r->operation.size()),0};
        Sink sink;
        const auto status=retiring?r->entry->cancel(r->entry->context,&op):r->entry->poll(r->entry->context,&op,&Sink::Write,&sink);
        std::scoped_lock lock(mutex);r->executing=false;
        if (retiring) {
            if (status==FUSIONMCP_OK) Release(*r);
        } else if (status==FUSIONMCP_PENDING && sink.calls==0) {
            Refresh(*r);
        } else if (status==FUSIONMCP_OK && !sink.failed && sink.calls==1) {
            Final(*r,status,sink);Release(*r);
        } else {
            Terminal(*r,"failed",Error("provider_contract","Async poll failed; cancellation acknowledgement required"));
        }
        r->nextPoll=now()+std::chrono::milliseconds(10);
        return Outcome::Success();
    }
    void Invalidate(const std::shared_ptr<Entry>& entry,const char* code) {
        for (const auto& [id,r]:requests) if (r->entry==entry && !r->terminal) {
            Terminal(*r,"cancelled",Error(code,"Provider disabled; cancellation cannot undo already-started effects"));
            if (r->jobQueued && !r->executing) {broker.Cancel(r->job);r->jobQueued=false;}
        }
    }
    Json Snapshot(RequestData& r) {
        Refresh(r);
        Json value={{"requestId",r.id},{"state",r.phase},{"epoch",r.epoch},{"started",r.started},
            {"invalidatedWhileRunning",r.invalidatedWhileRunning},{"retirementPending",r.leased&&r.terminal}};
        if (r.terminal) value["outcome"]=r.outcome;
        return Success(std::move(value));
    }
};

ProviderRegistry::ProviderRegistry(RequestBroker::NowFn now):state_(std::make_shared<State>(std::move(now))) {}
ProviderRegistry::~ProviderRegistry() {Stop();}
FUSIONMCP_Status ProviderRegistry::Register(const FUSIONMCP_ProviderV1* p,FUSIONMCP_ProviderHandle* handle) noexcept {
    return Add(p,handle,false,{});
}
FUSIONMCP_Status ProviderRegistry::RegisterBuiltin(const FUSIONMCP_ProviderV1* p,std::vector<std::string> capabilities) noexcept {
    FUSIONMCP_ProviderHandle handle=0;
    return Add(p,&handle,true,std::move(capabilities));
}
bool ProviderRegistry::RegisterControl(ControlTool tool) noexcept {
    try {
        if (tool.name.find('.')==tool.name.npos||!Identifier(tool.name,true)||!tool.handler||!SchemaSupported(tool.schema)||
            tool.schema.value("type",std::string())!="object"||tool.schema.value("additionalProperties",true)!=false) return false;
        std::scoped_lock lock(state_->mutex);
        if (state_->stopped||tool.name=="fusionmcp.status"||tool.name=="fusionmcp.request.get"||tool.name=="fusionmcp.request.cancel") return false;
        for (const auto& c:state_->controls) if (c.name==tool.name) return false;
        for (const auto& [id,p]:state_->providers) {
            if (p->nameSpace!="fusionmcp"&&p->nameSpace==Namespace(tool.name)) return false;
            for (const auto& c:p->commands) if (p->nameSpace+"."+c.name==tool.name) return false;
        }
        state_->controls.push_back(std::move(tool));
        return true;
    } catch (...) {return false;}
}
FUSIONMCP_Status ProviderRegistry::Add(const FUSIONMCP_ProviderV1* p,FUSIONMCP_ProviderHandle* handle,bool builtin,std::vector<std::string> capabilities) noexcept {
    try {
        if (!p||!handle) return FUSIONMCP_INVALID_ARGUMENT;
        *handle=0;
        if (p->version!=FUSIONMCP_ABI_V1||p->size!=sizeof(*p)) return FUSIONMCP_INCOMPATIBLE;
        if (!p->name_space||!p->invoke||!p->commands||!p->command_count||p->command_count>64||p->reserved||
            !p->namespace_bytes||p->namespace_bytes>32) return FUSIONMCP_INVALID_ARGUMENT;
        auto entry=std::make_shared<State::Entry>();
        entry->nameSpace.assign(p->name_space,p->namespace_bytes);
        if (!Identifier(entry->nameSpace)||(entry->nameSpace=="fusionmcp")!=builtin) return FUSIONMCP_INVALID_ARGUMENT;
        entry->builtin=builtin;entry->enabled=builtin;entry->supported=std::move(capabilities);
        entry->context=p->context;entry->invoke=p->invoke;entry->poll=p->poll;entry->cancel=p->cancel;entry->capabilities=p->capabilities;
        std::set<std::string> names;
        std::size_t metadataBytes=entry->nameSpace.size();
        for (uint32_t i=0;i<p->command_count;++i) {
            const auto& c=p->commands[i];
            if (c.version!=FUSIONMCP_ABI_V1||c.size!=sizeof(c)) return FUSIONMCP_INCOMPATIBLE;
            if (!c.name||!c.name_bytes||c.name_bytes>96||!c.description||c.description_bytes>4096||
                !c.input_schema||!c.input_schema_bytes||c.input_schema_bytes>65536||
                (c.flags&~(FUSIONMCP_COMMAND_MUTATING|FUSIONMCP_COMMAND_ASYNC))||
                (c.required_capabilities&~p->capabilities)||((c.flags&FUSIONMCP_COMMAND_ASYNC)&&(!p->poll||!p->cancel))) return FUSIONMCP_INVALID_ARGUMENT;
            State::Command command{std::string(c.name,c.name_bytes),std::string(c.description,c.description_bytes),
                ParseBounded(std::string_view(c.input_schema,c.input_schema_bytes),65536),ToOwner(c.owner),c.flags,c.required_capabilities};
            const bool controlName=builtin&&(command.name=="status"||command.name=="request.get"||command.name=="request.cancel");
            if (!Identifier(command.name,true)||controlName||!names.insert(command.name).second||!SchemaSupported(command.schema)||
                command.schema.value("type",std::string())!="object"||!command.schema.contains("additionalProperties")||
                command.schema["additionalProperties"]!=false) return FUSIONMCP_INVALID_ARGUMENT;
            (void)Json(command.description).dump();
            metadataBytes+=c.name_bytes+c.description_bytes+c.input_schema_bytes;
            if (metadataBytes>262144) return FUSIONMCP_INVALID_ARGUMENT;
            entry->commands.push_back(std::move(command));
        }
        std::scoped_lock lock(state_->mutex);
        if (state_->stopped) return FUSIONMCP_UNAVAILABLE;
        std::size_t commands=3+state_->controls.size();
        for (const auto& [id,existing]:state_->providers) {if (existing->nameSpace==entry->nameSpace) return FUSIONMCP_INVALID_ARGUMENT;commands+=existing->commands.size();}
        // Host-internal control namespaces (fusionmcp aside, which builtins share) stay host-owned.
        for (const auto& c:state_->controls) if (Namespace(c.name)!="fusionmcp"&&Namespace(c.name)==entry->nameSpace) return FUSIONMCP_INVALID_ARGUMENT;
        if (state_->providers.size()>=16||commands+entry->commands.size()>128||state_->nextProvider==std::numeric_limits<uint64_t>::max()) return FUSIONMCP_BUSY;
        for (const auto& command:entry->commands)
            if (state_->DescribeCommand(*entry,command,true).dump().size()>kMaxDescriptorBytes) return FUSIONMCP_BUSY;
        // Serialized aggregate cap includes escaped strings, schemas and host
        // metadata. availability=false is the larger representation, including
        // existing enabled providers. Lifecycle changes cannot grow beyond it.
        // A refused provider cannot break core discovery.
        if (state_->Catalog(entry.get(),true).dump().size()>kMaxCatalogBytes) return FUSIONMCP_BUSY;
        entry->handle=state_->nextProvider++;state_->providers.emplace(entry->handle,entry);*handle=entry->handle;return FUSIONMCP_OK;
    } catch (...) {return FUSIONMCP_INVALID_ARGUMENT;}
}
FUSIONMCP_Status ProviderRegistry::SetEnabled(FUSIONMCP_ProviderHandle handle,bool enabled) noexcept {
    try {
        std::scoped_lock lock(state_->mutex);const auto it=state_->providers.find(handle);
        if (it==state_->providers.end()||it->second->builtin||state_->stopped) return FUSIONMCP_UNAVAILABLE;
        auto& p=*it->second;
        // A quarantined provider cannot be made live again by toggling enable.
        for (const auto& [id,r]:state_->requests) if (r->entry==it->second&&r->quarantined) return FUSIONMCP_BUSY;
        if (p.enabled==enabled) return FUSIONMCP_OK;
        if (p.generation==std::numeric_limits<uint64_t>::max()) return FUSIONMCP_BUSY;
        p.enabled=enabled;++p.generation;
        if (!enabled) state_->Invalidate(it->second,"provider_disabled");
        return FUSIONMCP_OK;
    } catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
}
FUSIONMCP_Status ProviderRegistry::Unregister(FUSIONMCP_ProviderHandle handle) noexcept {
    try {
        std::scoped_lock lock(state_->mutex);const auto it=state_->providers.find(handle);
        if (it==state_->providers.end()||it->second->builtin) return FUSIONMCP_UNAVAILABLE;
        it->second->enabled=false;if (it->second->generation!=std::numeric_limits<uint64_t>::max()) ++it->second->generation;
        state_->Invalidate(it->second,"provider_unregistered");
        if (it->second->leases) return FUSIONMCP_BUSY;
        state_->providers.erase(it);return FUSIONMCP_OK;
    } catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
}
void ProviderRegistry::SetOwnerAvailable(Owner owner,bool available) {
    std::scoped_lock lock(state_->mutex);state_->owners.at(static_cast<std::size_t>(owner))=available;
}
void ProviderRegistry::SetMutationsAllowed(bool allowed) {
    std::scoped_lock lock(state_->mutex);state_->mutations=allowed;
    if (!allowed) for (const auto& [id,r]:state_->requests) if (r->command.flags&FUSIONMCP_COMMAND_MUTATING) {
        state_->Terminal(*r,"cancelled",Error("read_only","Mutation authorization revoked; retiring operation"));
        if (r->jobQueued&&!r->executing) {state_->broker.Cancel(r->job);r->jobQueued=false;}
    }
}
Json ProviderRegistry::Describe() const {
    std::scoped_lock lock(state_->mutex);
    return state_->Catalog();
}
Json ProviderRegistry::Lookup(const std::string& name) const {
    std::scoped_lock lock(state_->mutex);
    if (name=="fusionmcp.status") return Descriptor(name,"Standalone capabilities and lifecycle status",EmptySchema());
    if (name=="fusionmcp.request.get") return Descriptor(name,"Actual completion and retirement status",IdSchema());
    if (name=="fusionmcp.request.cancel") return Descriptor(name,"Request cancellation; cannot undo already-started effects",IdSchema(),true);
    for (const auto& c:state_->controls) if (c.name==name) return DescribeControl(c);
    const auto dot=name.find('.');
    if (dot==name.npos) return Json();
    const auto ns=name.substr(0,dot),local=name.substr(dot+1);
    for (const auto& [id,p]:state_->providers) if (p->nameSpace==ns) {
        const auto c=std::find_if(p->commands.begin(),p->commands.end(),[&](const auto& command){return command.name==local;});
        return c==p->commands.end()?Json():state_->DescribeCommand(*p,*c);
    }
    return Json();
}
Json ProviderRegistry::Invoke(const std::string& name,const std::string& arguments,bool allowMutations) {
    Json args;
    try {args=ParseBounded(arguments,65536);} catch (...) {return Error("invalid_arguments","Malformed bounded JSON arguments");}
    if (name=="fusionmcp.status") {
        if (!MatchesSchema(args,EmptySchema())) return Error("invalid_arguments","Invalid status arguments");
        std::scoped_lock lock(state_->mutex);
        Json supported=Json::array({"catalog","provider_lifecycle","owner_requests","async_provider_poll_cancel"});
        Json unsupported=Json::array();
        Json capabilities=Json::object();
        std::size_t external=0;
        for (const auto& [handle,p]:state_->providers) {
            if (!p->builtin) {++external;continue;}
            for (const auto& capability:p->supported) {
                supported.push_back(capability);
                capabilities[capability]={{"available",p->enabled&&!p->commands.empty()&&state_->Available(p->commands.front().owner)},
                    {"owner",OwnerName(p->commands.front().owner)}};
            }
        }
        for (const auto& c:state_->controls) if (!c.capability.empty()&&!capabilities.contains(c.capability)) {
            supported.push_back(c.capability);
            const auto reason=UnavailableReason(c);
            capabilities[c.capability]={{"available",reason.empty()},{"owner","control"}};
            if (!reason.empty()) capabilities[c.capability]["reason"]=reason;
        }
        for (const auto* capability:{"native_console","named_save_load","save_readiness","ordered_batches","native_hdr_screenshot","scene_inspection","papyrus"})
            if (std::find(supported.begin(),supported.end(),capability)==supported.end()) unsupported.push_back(capability);
        if (capabilities.contains("native_console"))
            capabilities["native_console"]["completion"]="executed by the engine before the result; output is the console print text for the line; semantic success is read from the output";
        if (capabilities.contains("game_readiness_sample"))
            capabilities["game_readiness_sample"]["completion"]="single owner sample; not a load barrier";
        return Success({{"mod","FusionMCP"},{"epoch",state_->broker.Epoch()},{"providerCount",external},
            {"mutationsAllowed",state_->mutations},{"supported",std::move(supported)},{"unsupported",std::move(unsupported)},
            {"capabilities",std::move(capabilities)}});
    }
    if (name=="fusionmcp.request.get"||name=="fusionmcp.request.cancel") {
        if (!MatchesSchema(args,IdSchema())) return Error("invalid_arguments","Invalid request arguments");
        if (name=="fusionmcp.request.get") return Query(args["requestId"].get<uint64_t>());
        {std::scoped_lock lock(state_->mutex);if (!allowMutations||!state_->mutations) return Error("read_only","Mutations disabled");}
        return Cancel(args["requestId"].get<uint64_t>());
    }
    std::function<Json(const Json&,bool)> control;
    {
        std::scoped_lock lock(state_->mutex);
        const auto c=std::find_if(state_->controls.begin(),state_->controls.end(),[&](const auto& tool){return tool.name==name;});
        if (c!=state_->controls.end()) {
            if (state_->stopped) return Error("stopped","Core stopped");
            if (const auto reason=UnavailableReason(*c);!reason.empty()) return Error("unavailable",reason);
            if (Mutates(*c,args)&&(!allowMutations||!state_->mutations)) return Error("read_only","Mutations disabled");
            if (!MatchesSchema(args,c->schema)) return Error("invalid_arguments","Arguments do not match schema");
            control=c->handler;
        }
    }
    if (control) {
        try {
            auto result=control(args,allowMutations&&state_->mutations);
            return Envelope(result)?result:Error("core_error","Control tool returned an invalid envelope");
        } catch (...) {return Error("core_error","Control tool failed");}
    }
    std::scoped_lock lock(state_->mutex);
    if (state_->stopped) return Error("stopped","Core stopped");
    const auto dot=name.find('.');
    const auto ns=name.substr(0,dot);const auto local=dot==name.npos?std::string():name.substr(dot+1);
    std::shared_ptr<State::Entry> provider;
    for (const auto& [id,p]:state_->providers) if (p->nameSpace==ns) {provider=p;break;}
    if (!provider) return Error("unsupported","Command is not registered");
    const auto command=std::find_if(provider->commands.begin(),provider->commands.end(),[&](const auto& c){return c.name==local;});
    if (command==provider->commands.end()) return Error("unsupported","Command is not registered");
    if (!provider->enabled||!state_->Available(command->owner)) return Error("unavailable","Provider or actual owner adapter is unavailable");
    if ((command->flags&FUSIONMCP_COMMAND_MUTATING)&&(!allowMutations||!state_->mutations)) return Error("read_only","Mutations disabled");
    if (!MatchesSchema(args,command->schema)) return Error("invalid_arguments","Arguments do not match schema");
    // Retain at most 128 records; only fully retired terminal records may leave.
    while (state_->requests.size()>=128) {
        const auto victim=std::find_if(state_->requests.begin(),state_->requests.end(),[](const auto& x){return x.second->terminal&&!x.second->leased&&!x.second->executing&&!x.second->jobQueued;});
        if (victim==state_->requests.end()) return Error("queue_full","Request history is full");
        state_->requests.erase(victim);
    }
    std::size_t live=0;
    for (const auto& [id,r]:state_->requests) if (!r->terminal||r->leased) ++live;
    if (live>=32||state_->nextRequest==std::numeric_limits<uint64_t>::max()) return Error("queue_full","Outstanding operation limit");
    auto r=std::make_shared<State::RequestData>();r->id=state_->nextRequest++;r->entry=provider;r->generation=provider->generation;
    r->epoch=state_->broker.Epoch();r->command=*command;r->arguments=args.dump();r->deadline=state_->now()+RequestDeadline(args);
    const auto state=state_;
    const auto job=state_->broker.Reserve(command->owner,[state,r]{return state->Start(r);},r->epoch,r->deadline);
    if (!job.accepted) return Error(SubmitReasonName(job.reason),"Owner queue admission refused");
    r->job=job.id;r->jobQueued=true;state_->requests.emplace(r->id,r);
    state_->broker.Activate(job.id,r->epoch);
    return Success({{"requestId",r->id},{"state","queued"},{"epoch",r->epoch},{"completed",false}});
}
Json ProviderRegistry::Query(RequestId id) {
    std::scoped_lock lock(state_->mutex);const auto it=state_->requests.find(id);
    return it==state_->requests.end()?Error("not_found","Request not retained"):state_->Snapshot(*it->second);
}
Json ProviderRegistry::Cancel(RequestId id) {
    std::scoped_lock lock(state_->mutex);const auto it=state_->requests.find(id);
    if (it==state_->requests.end()) return Error("not_found","Request not retained");
    auto& r=*it->second;
    state_->Terminal(r,"cancelled",Error("cancelled","Cancellation cannot undo already-started effects"));
    if (r.jobQueued&&!r.executing) {state_->broker.Cancel(r.job);r.jobQueued=false;}
    return state_->Snapshot(r);
}
std::size_t ProviderRegistry::Pump(Owner owner,std::size_t budget,EpochId claimedEpoch) {
    if (!budget) return 0;
    budget=std::min<std::size_t>(budget,4);
    EpochId epoch;
    {
        std::scoped_lock lock(state_->mutex);
        if (!state_->Available(owner)) return 0;
        epoch=state_->broker.Epoch();
        if (claimedEpoch!=0&&claimedEpoch!=epoch) return 0;
        std::size_t submitted=0;
        auto& cursor=state_->pollCursor.at(static_cast<std::size_t>(owner));
        auto it=state_->requests.upper_bound(cursor);
        const auto count=state_->requests.size();
        for (std::size_t visited=0;visited<count;++visited) {
            if (it==state_->requests.end()) it=state_->requests.begin();
            const auto id=it->first;const auto r=(it++)->second;
            state_->Refresh(*r);
            if (r->command.owner!=owner||!r->leased||r->quarantined||r->executing||r->jobQueued||state_->now()<r->nextPoll) continue;
            const auto state=state_;
            const auto job=state_->broker.Reserve(owner,[state,r]{return state->Continue(r);},epoch,state_->now()+std::chrono::seconds(1));
            if (!job.accepted) break;
            r->job=job.id;r->jobQueued=true;state_->broker.Activate(job.id,epoch);
            cursor=id;
            if (++submitted==budget) break;
        }
    }
    return state_->broker.Pump(owner,budget,epoch);
}
EpochId ProviderRegistry::AdvanceEpoch() {
    std::scoped_lock lock(state_->mutex);const auto epoch=state_->broker.AdvanceEpoch();
    for (const auto& [id,r]:state_->requests) {
        state_->Terminal(*r,"interrupted",Error("epoch_changed","Load epoch changed; old work will not continue"));
        if (!r->executing) r->jobQueued=false;
    }
    return epoch;
}
EpochId ProviderRegistry::Epoch() const {return state_->broker.Epoch();}
void ProviderRegistry::Stop() {
    std::scoped_lock lock(state_->mutex);
    if (state_->stopped) return;
    state_->stopped=true;state_->mutations=false;
    for (const auto& [id,p]:state_->providers) {p->enabled=false;++p->generation;state_->Invalidate(p,"stopped");}
    // Preserve owner retirement pumping for existing async leases after stop.
    state_->broker.AdvanceEpoch();
}
std::size_t ProviderRegistry::Pending(Owner owner) const {
    std::scoped_lock lock(state_->mutex);std::size_t count=0;
    for (const auto& [id,r]:state_->requests) if (r->command.owner==owner&&(!r->terminal||r->leased)) ++count;
    return count;
}
}
