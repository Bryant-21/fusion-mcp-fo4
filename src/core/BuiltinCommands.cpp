#include "BuiltinCommands.h"
#include "Artifact.h"
#include "ConsoleCapture.h"
#include "ConsoleCatalog.h"
#include "FrameCapture.h"
#include "Json.h"
#include "WorldCommands.h"
#include "LogTools.h"
#include "TestCommands.h"
#include <fstream>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <thread>

namespace fusionmcp {
namespace {
constexpr const char* kExecuteSchema=R"({"type":"object","properties":{
"command":{"type":"string","minLength":1,"maxLength":64,"description":"Native console command name or alias listed by fusionmcp.console.commands"},
"target":{"type":"string","minLength":1,"maxLength":8,"description":"\"player\" or an 8-digit hexadecimal runtime FormID; required when referenceFunction is true, optional otherwise"},
"arguments":{"type":"array","maxItems":16,"items":{"oneOf":[{"type":"string","minLength":1,"maxLength":256},{"type":"number"}]}},
"waitMs":{"type":"integer","minimum":0,"maximum":30000,"description":"Keep recording console output for up to this long after the line ran, until 500 ms pass without output"}},
"required":["command"],"additionalProperties":false})";
constexpr const char* kRunSchema=R"({"type":"object","properties":{
"line":{"type":"string","minLength":1,"maxLength":1000,"description":"One console line exactly as typed into the console, e.g. set gamehour to 12, player.GetAV Health, help iron 4, load MySave, bat mybatch; ';' separates statements"},
"waitMs":{"type":"integer","minimum":0,"maximum":30000,"description":"Keep recording console output for up to this long after the line ran, until 500 ms pass without output. Default 0; 3000 for bat/RunConsoleBatch. Ignored for load lines"}},
"required":["line"],"additionalProperties":false})";
constexpr const char* kListSchema=R"({"type":"object","properties":{
"filter":{"type":"string","maxLength":64,"description":"Case-insensitive name or alias substring"},
"offset":{"type":"integer","minimum":0,"maximum":4096},
"limit":{"type":"integer","minimum":1,"maximum":200}},"additionalProperties":false})";
constexpr const char* kLoadSchema=R"({"type":"object","properties":{
"save":{"type":"string","minLength":1,"maxLength":200,"description":"Exact save name from fusionmcp.saves.list: the file name without its .fos extension"}},
"required":["save"],"additionalProperties":false})";
constexpr const char* kEmptySchema=R"({"type":"object","properties":{},"additionalProperties":false})";
constexpr const char* kScreenshotSchema=R"({"type":"object","properties":{"hideHud":{"type":"boolean"},
"face":{"type":"string","minLength":1,"maxLength":128,"description":"Reference to turn the player toward before capturing"},
"method":{"type":"string","enum":["native","engine"],"description":"native (default): read the presented frame back from D3D11 into the FusionMCP screenshot folder; engine: the game's own ScreenShot command, which writes ScreenShotN.png into the game folder"}},"additionalProperties":false})";
// Wall-clock bound for capture.image capture=true; the capture's own frame and
// encode limits normally end it much sooner.
constexpr auto kNativeImageWait=std::chrono::seconds(30);
constexpr const char* kLogReadSchema=R"j({"type":"object","properties":{
"name":{"type":"string","minLength":1,"maxLength":128,"description":"Log name from fusionmcp.logs.list; '*' picks the newest match (F4SE/F4FS*.log)"},
"offset":{"type":"integer","minimum":0,"description":"Byte offset to read from (a previous nextOffset); default: the last maxBytes"},
"since":{"type":"string","minLength":1,"maxLength":200,"description":"Marker or stored marker id from fusionmcp.logs.mark: read only what was appended after it"},
"maxBytes":{"type":"integer","minimum":1,"maximum":65536},
"contains":{"type":"string","minLength":1,"maxLength":128,"description":"Keep only lines containing this text (case-insensitive)"}},
"additionalProperties":false})j";
constexpr const char* kLogMarkSchema=R"j({"type":"object","properties":{
"name":{"type":"string","minLength":1,"maxLength":128,"description":"Log name from fusionmcp.logs.list; '*' picks the newest match"},
"id":{"type":"string","minLength":1,"maxLength":64,"description":"Store the marker under this id so later calls (and batch steps) can pass since=id"}},
"required":["name"],"additionalProperties":false})j";

FUSIONMCP_Status Write(FUSIONMCP_WriteFn writer,void* sink,const Json& envelope) {
    const auto bytes=envelope.dump();
    if (bytes.size()>FUSIONMCP_MAX_RESULT_BYTES) return FUSIONMCP_INTERNAL_FAILURE;
    return writer(sink,bytes.data(),static_cast<uint32_t>(bytes.size()))?FUSIONMCP_OK:FUSIONMCP_WRITER_FAILED;
}
// The console log is engine text of unknown encoding; JSON needs valid UTF-8.
std::string Lower(std::string text) {
    for (char& c:text) if (c>='A'&&c<='Z') c=static_cast<char>(c+('a'-'A'));
    return text;
}
std::string PrintableText(std::string text) {
    for (char& c:text) {
        const auto u=static_cast<unsigned char>(c);
        if ((u<32&&c!='\n'&&c!='\t')||u>126) c='?';
    }
    return text;
}
// Same engine constraints as console arguments, plus file-name characters.
bool SafeSaveName(std::string_view name) {
    return !name.empty()&&name.size()<=200&&std::all_of(name.begin(),name.end(),[](unsigned char c) {
        return c>=32&&c<=126&&!std::strchr("\"%[]\\/:*?<>|",c);
    });
}
Json Schema(const char* text) {return ParseBounded(text);}
// Off-owner file read. Only names the adapter listed are readable.
Json ReadLog(const std::vector<FileEntry>& logs,const LogMarkers& markers,const Json& args) {
    std::string wanted=args.value("name",std::string());
    std::optional<std::uint64_t> since;
    if (args.contains("since")) {
        if (args.contains("offset")) return Error("invalid_arguments","Pass offset or since, not both");
        const auto marker=markers.Find(args["since"].get<std::string>());
        if (!marker) return Error("not_found","since is neither a marker (<log>#<offset>) nor a stored marker id");
        if (!wanted.empty()&&!WildcardMatch(wanted,marker->log)) return Error("invalid_arguments","since marks a different log than name");
        wanted=marker->log;since=marker->offset;
    }
    if (wanted.empty()) return Error("invalid_arguments","Give name or since");
    const auto found=ResolveLog(logs,wanted);
    if (!found) return Error("not_found","No listed log has this name");
    const auto* it=&*found;
    std::ifstream file(std::filesystem::path(it->path),std::ios::binary);
    if (!file) return Error("unavailable","Log cannot be opened");
    file.seekg(0,std::ios::end);
    const auto size=static_cast<std::uint64_t>(std::max<std::streamoff>(0,file.tellg()));
    const auto maxBytes=args.value("maxBytes",16384ull);
    std::uint64_t offset=since?*since:args.contains("offset")?args["offset"].get<std::uint64_t>():(size>maxBytes?size-maxBytes:0);
    bool reset=false;
    if (offset>size) {offset=0;reset=true;}
    const auto count=std::min<std::uint64_t>(maxBytes,size-offset);
    std::string text(static_cast<std::size_t>(count),'\0');
    file.seekg(static_cast<std::streamoff>(offset));
    file.read(text.data(),static_cast<std::streamsize>(count));
    text.resize(static_cast<std::size_t>(file.gcount()));
    if (args.contains("contains")) {
        const auto needle=Lower(args["contains"].get<std::string>());
        std::string kept;
        for (std::size_t start=0;start<text.size();) {
            auto end=text.find('\n',start);
            if (end==std::string::npos) end=text.size();
            const auto line=text.substr(start,end-start);
            if (Lower(line).find(needle)!=std::string::npos) kept+=line+"\n";
            start=end+1;
        }
        text=std::move(kept);
    }
    std::erase(text,'\r');
    const auto next=offset+count;
    return Success({{"name",PrintableText(it->name)},{"size",size},{"offset",offset},{"nextOffset",next},
        {"marker",FormatMarker({it->name,next})},{"more",next<size},{"rotated",reset},{"text",PrintableText(std::move(text))}});
}
struct HashJob {
    std::atomic_bool done{false};
    ArtifactInfo info;
};
std::string NativeUnavailable(FrameCapture* frames) {
    return "Native frame capture is unavailable ("+(frames?frames->Detail():std::string("no present hook on this runtime"))+
        "); pass {\"method\":\"engine\"} to use the engine ScreenShot command instead";
}
Json NativeArtifact(const FrameCapture::Status& s) {
    const auto& a=s.artifact;
    return {{"name",s.name},{"path",PrintableText(s.path)},{"mimeType","image/png"},{"bytes",a.bytes},{"width",a.width},
        {"height",a.height},{"bitDepth",a.bitDepth},{"colorType",a.colorType},{"sha256",a.sha256}};
}
}

struct BuiltinCommands::State {
    enum class Kind {Console,Screenshot};
    struct Operation {
        Kind kind=Kind::Console;
        std::string line;
        unsigned polls=0,stable=0;
        RequestTime dispatched;
        // Console: capture generation, recording window and the result fields.
        std::uint64_t capture=0,prints=0;
        std::chrono::milliseconds window{0};
        RequestTime lastPrint;
        Json result;
        std::optional<ConsoleCapture::Snapshot> frozen;
        std::set<std::string> before;
        FileEntry candidate;
        std::shared_ptr<HashJob> job;
        unsigned settlePolls=0;
        bool restoreHud=false,hidHud=false,native=false;
        std::uint64_t frameId=0;
    };
    std::shared_ptr<GameServices> services;
    RequestBroker::NowFn now;
    SaveLoadCoordinator saves;
    std::vector<std::string> names,descriptions;
    std::vector<FUSIONMCP_CommandV1> commands;
    FUSIONMCP_ProviderV1 provider{};
    std::mutex mutex;
    std::optional<ConsoleCatalog> catalog;
    std::map<std::string,Operation> operations;
    std::unique_ptr<WorldCommands> world;
    LogMarkers markers;
    std::unique_ptr<TestCommands> tests;

    explicit State(RequestBroker::NowFn clock):now(clock),saves(clock) {}
    void Add(std::string name,std::string description,const char* schema,uint32_t flags) {
        names.push_back(std::move(name));descriptions.push_back(std::move(description));
        commands.push_back({FUSIONMCP_ABI_V1,sizeof(FUSIONMCP_CommandV1),nullptr,0,nullptr,0,schema,static_cast<uint32_t>(std::strlen(schema)),
            FUSIONMCP_OWNER_GAME,flags,0});
    }
    const ConsoleCatalog& Catalog() {
        if (!catalog) catalog.emplace(services->ConsoleCommands());
        return *catalog;
    }
    FUSIONMCP_Status ListCommands(const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
        const auto& table=Catalog();
        if (!table.Size()) return Write(writer,sink,Error("unavailable","Native console command table is empty or unreadable"));
        return Write(writer,sink,Success(table.List(args.value("filter",std::string()),args.value("offset",0u),args.value("limit",50u))));
    }
    FUSIONMCP_Status Execute(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
        const auto rows=Catalog().Candidates(args["command"].get<std::string>());
        if (rows.empty()) return Write(writer,sink,Error("unknown_console_command","No native console command has this name or alias"));
        const auto row=MergeRows(rows);
        if (!row) return Write(writer,sink,Error("ambiguous_console_command",std::to_string(rows.size())+
            " native rows with different names match this alias; pass the full name, or use fusionmcp.console.run"));
        const auto line=AssembleConsoleLine(*row,args);
        if (!line.errorCode.empty()) return Write(writer,sink,Error(line.errorCode,line.errorMessage));
        return RunLine(i,line.text,row->name,args,writer,sink);
    }
    // A pending console observation stops recording when a later line starts.
    void FreezeConsoleOperations() {
        auto* capture=services->Capture();
        for (auto& [id,o]:operations) if (o.kind==Kind::Console&&o.capture&&!o.frozen) {
            o.frozen=capture?capture->Read(o.capture):ConsoleCapture::Snapshot{};
            if (capture) capture->End(o.capture);
        }
    }
    Json LoadTransaction(const FUSIONMCP_InvocationV1& i,const ConsoleStatement& statement,std::uint64_t& id) {
        if (statement.argument.empty()) return {{"transactionId",nullptr},{"reason","No save name; the engine reports its own usage"}};
        const auto validated=saves.Validate(statement.argument,services->ListSaves());
        if (!validated["ok"].get<bool>())
            return {{"transactionId",nullptr},{"save",PrintableText(statement.argument)},{"reason",validated["error"]["message"]}};
        const auto save=validated["result"]["save"].get<std::string>();
        id=saves.Begin(save,i.load_epoch);
        return {{"transactionId",id},{"save",save},{"phase","dispatched"},
            {"note","Poll fusionmcp.wait_ready with this transactionId until ready or a terminal phase, as after fusionmcp.saves.load"}};
    }
    // One console line on the Game owner: the engine runs it before this returns.
    FUSIONMCP_Status RunLine(const FUSIONMCP_InvocationV1& i,const std::string& text,const std::string& command,const Json& args,
            FUSIONMCP_WriteFn writer,void* sink) {
        const auto checked=ValidateConsoleLine(text);
        if (!checked.errorCode.empty()) return Write(writer,sink,Error(checked.errorCode,checked.errorMessage));
        const auto statement=ParseConsoleStatement(text);
        const auto rows=Catalog().Candidates(statement.command);
        const bool load=!rows.empty()&&IsLoadCommand(*rows.front())&&statement.single&&statement.target.empty();
        const bool batch=!rows.empty()&&IsBatchCommand(*rows.front());
        // A load raises F4SE load messages inside the engine call, which retire
        // every request of the old epoch, this one included. Lines that load are
        // left queued for the engine's own drain on the next frame instead.
        bool loads=false;
        for (std::string_view rest=text;!loads;) {
            const auto part=ParseConsoleStatement(rest);
            const auto candidates=Catalog().Candidates(part.command);
            loads=!candidates.empty()&&IsLoadCommand(*candidates.front());
            const auto next=rest.find(';');
            if (next==std::string_view::npos) break;
            rest.remove_prefix(next+1);
        }
        std::uint64_t transaction=0;
        Json loadInfo=load?LoadTransaction(i,statement,transaction):Json();
        FreezeConsoleOperations();
        auto run=services->RunConsoleLine(text,!loads);
        if (transaction) {
            saves.Dispatched(transaction,run.dispatched);
            if (!run.dispatched) loadInfo["phase"]="failed";
        }
        if (!run.dispatched) return Write(writer,sink,Error(run.errorCode,run.errorMessage));
        Operation o;o.kind=Kind::Console;o.line=text;o.dispatched=o.lastPrint=now();o.capture=run.capture;
        // A load changes the load epoch, which retires pending requests: report at once.
        o.window=std::chrono::milliseconds(load?0u:args.value("waitMs",batch?kBatchWindowMs:0u));
        o.result={{"line",PrintableText(text)},{"completion",loads?"queued":"executed"},{"semanticSuccess",nullptr},{"outputSource",run.source}};
        if (loads) o.window=std::chrono::milliseconds(0);
        if (!command.empty()) o.result["command"]=command;
        if (load) o.result["load"]=std::move(loadInfo);
        if (!rows.empty()&&IsLifecycleCommand(*rows.front())) o.result["lifecycle"]=true;
        if (!o.capture||o.window.count()==0) {
            if (o.capture) services->Capture()->End(o.capture);
            return Write(writer,sink,Success(ConsoleResult(o,run.output,run.truncated,true)));
        }
        const auto id="console-"+std::to_string(i.request_id);
        operations[id]=std::move(o);
        return writer(sink,id.data(),static_cast<uint32_t>(id.size()))?FUSIONMCP_PENDING:FUSIONMCP_WRITER_FAILED;
    }
    Json ConsoleResult(const Operation& o,std::string output,bool truncated,bool settled) {
        if (output.size()>kMaxConsoleOutputBytes) {output.resize(kMaxConsoleOutputBytes);truncated=true;}
        auto result=o.result;
        result["output"]=PrintableText(std::move(output));result["outputTruncated"]=truncated;result["settled"]=settled;
        result["observation"]={{"windowMs",o.window.count()},{"prints",o.prints},
            {"elapsedMs",std::chrono::duration_cast<std::chrono::milliseconds>(now()-o.dispatched).count()}};
        return result;
    }
    FUSIONMCP_Status LoadSave(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
        const auto requested=args["save"].get<std::string>();
        if (!SafeSaveName(requested)) return Write(writer,sink,Error("invalid_arguments","Save names are printable ASCII without quote, percent, brackets or path characters"));
        const auto validated=saves.Validate(requested,services->ListSaves());
        if (!validated["ok"].get<bool>()) return Write(writer,sink,validated);
        const auto save=validated["result"]["save"].get<std::string>();
        // The dedicated wrapper still refuses duplicate canonical LoadGame rows.
        const auto match=Catalog().Resolve("LoadGame");
        if (match.status==ConsoleResolution::Missing) return Write(writer,sink,Error("unavailable","Native LoadGame command is not in the console table"));
        if (match.status==ConsoleResolution::Ambiguous)
            return Write(writer,sink,Error("ambiguous_console_command",std::to_string(match.candidates)+" native LoadGame rows; refusing to pick one"));
        const auto line=match.row->name+" "+(save.find(' ')==std::string::npos?save:'"'+save+'"');
        if (line.size()>kMaxConsoleLineBytes) return Write(writer,sink,Error("invalid_arguments","Load line exceeds the native buffer bound"));
        const auto id=saves.Begin(save,i.load_epoch);
        const bool dispatched=services->ExecuteConsole(line);
        saves.Dispatched(id,dispatched);
        if (!dispatched) return Write(writer,sink,Error("dispatch_failed","Native console refused the load line"));
        return Write(writer,sink,Success({{"transactionId",id},{"save",save},{"phase","dispatched"},{"ready",false},
            {"note","Dispatch is not a load. Poll fusionmcp.wait_ready until ready or a terminal phase."}}));
    }
    void RestoreHud(Operation& o) {
        if (o.restoreHud&&services->World()) services->World()->SetHudVisible(true);
        o.restoreHud=false;
    }
    FUSIONMCP_Status Screenshot(const FUSIONMCP_InvocationV1& i,const Json& args,FUSIONMCP_WriteFn writer,void* sink) {
        Operation o;o.kind=Kind::Screenshot;o.dispatched=now();
        o.native=args.value("method",std::string("native"))=="native";
        auto* frames=services->Frames();
        if (o.native&&(!frames||!frames->Installed())) return Write(writer,sink,Error("unavailable",NativeUnavailable(frames)));
        auto* adapter=services->World();
        if ((args.contains("hideHud")||args.contains("face"))&&!adapter)
            return Write(writer,sink,Error("unsupported","Screenshot options need the adapter adapter"));
        if (args.contains("face")) {
            const auto form=adapter->ResolveForm(args["face"].get<std::string>());
            std::string reason;
            if (!form.found) return Write(writer,sink,Error("form_not_found","No loaded form matches face"));
            if (!adapter->FaceReference(form.formId,reason)) return Write(writer,sink,Error("camera_failed",reason));
            o.settlePolls=2;
        }
        if (args.value("hideHud",false)&&adapter->HudVisible()) {
            if (!adapter->SetHudVisible(false)) return Write(writer,sink,Error("capture_failed","HUD could not be hidden"));
            o.restoreHud=o.hidHud=true;o.settlePolls=2;
        }
        if (!o.native) for (const auto& file:services->ListScreenshots()) o.before.insert(file.name);
        // Hiding the HUD or turning the player shows only on a later frame.
        if (!o.settlePolls) {
            const auto error=Start(o);
            if (!error.is_null()) {RestoreHud(o);return Write(writer,sink,error);}
        }
        const auto id="capture-"+std::to_string(i.request_id);
        operations[id]=std::move(o);
        return writer(sink,id.data(),static_cast<uint32_t>(id.size()))?FUSIONMCP_PENDING:FUSIONMCP_WRITER_FAILED;
    }
    FUSIONMCP_Status Readiness(const FUSIONMCP_InvocationV1& i,FUSIONMCP_WriteFn writer,void* sink) {
        const auto s=services->SampleReadiness();
        const bool ready=s.sampled&&!s.loadingMenuOpen&&!s.mainMenuOpen&&s.playerPresent&&s.parentCellAttached&&s.player3DLoaded;
        return Write(writer,sink,Success({{"ready",ready},{"barrier",false},{"epoch",i.load_epoch},
            {"evidence",s.sampled?"sampled_on_game_owner":"unavailable"},{"reason",s.reason},
            {"observations",{{"loadingMenuOpen",s.loadingMenuOpen},{"mainMenuOpen",s.mainMenuOpen},{"playerPresent",s.playerPresent},
                {"parentCellAttached",s.parentCellAttached},{"player3DLoaded",s.player3DLoaded}}},
            {"note","Single sample; not a load barrier and not correlated with any requested save"}}));
    }
    FUSIONMCP_Status PollConsole(Operation& o,FUSIONMCP_WriteFn writer,void* sink) {
        ++o.polls;
        auto* capture=services->Capture();
        if (o.frozen) {
            auto result=ConsoleResult(o,o.frozen->text,o.frozen->truncated,false);
            result["supersededBy"]="next_console_line";
            return Write(writer,sink,Success(std::move(result)));
        }
        const auto snapshot=capture?capture->Read(o.capture):ConsoleCapture::Snapshot{};
        const auto at=now();
        if (snapshot.appends!=o.prints) {o.prints=snapshot.appends;o.lastPrint=at;}
        const bool quiet=o.polls>=2&&at-o.lastPrint>=kConsoleQuiet;
        if (!quiet&&at-o.dispatched<o.window&&snapshot.current) return FUSIONMCP_PENDING;
        if (capture) capture->End(o.capture);
        return Write(writer,sink,Success(ConsoleResult(o,snapshot.text,snapshot.truncated,quiet)));
    }
    FUSIONMCP_Status PollScreenshot(Operation& o,FUSIONMCP_WriteFn writer,void* sink) {
        const auto status=PollScreenshotStage(o,writer,sink);
        if (status!=FUSIONMCP_PENDING) RestoreHud(o);
        return status;
    }
    // Asks for the capture: a native frame request or the engine's screenshot flag.
    // Returns an error envelope, or null when the request is in flight.
    Json Start(Operation& o) {
        o.dispatched=now();
        if (!o.native) {
            if (services->QueueScreenshot()) return nullptr;
            return Error("capture_busy","Engine screenshot handler is unavailable or already has a queued capture");
        }
        auto* frames=services->Frames();
        if (!frames) return Error("unavailable",NativeUnavailable(frames));
        const auto admitted=frames->Request();
        if (!admitted.code.empty()) return Error(admitted.code,admitted.message);
        o.frameId=admitted.id;
        return nullptr;
    }
    FUSIONMCP_Status PollNative(Operation& o,FUSIONMCP_WriteFn writer,void* sink) {
        const auto status=services->Frames()->Poll(o.frameId);
        if (status.state==FrameCapture::State::Pending) return FUSIONMCP_PENDING;
        o.frameId=0;
        if (status.state==FrameCapture::State::Failed) return Write(writer,sink,Error(status.code,status.message));
        return Write(writer,sink,Success({{"artifact",NativeArtifact(status)},{"source","native_present_readback"},
            {"completion","file_verified"},{"hdr",false},{"hudHidden",o.hidHud},{"swapChainFormat",status.dxgiFormat},
            {"presentCount",status.presentCount},
            {"colorProcessing","Lossless copy of the swap chain image the game presented (game HUD included; F4FS's overlay is "
                "excluded behind its proxy presenter but included under its native presentation). 10-bit swap chains are saved "
                "as 16-bit PNG; HDR values are not preserved"}}));
    }
    FUSIONMCP_Status PollScreenshotStage(Operation& o,FUSIONMCP_WriteFn writer,void* sink) {
        if (o.settlePolls) {
            if (--o.settlePolls) return FUSIONMCP_PENDING;
            const auto error=Start(o);
            return error.is_null()?FUSIONMCP_PENDING:Write(writer,sink,error);
        }
        if (o.native) return PollNative(o,writer,sink);
        if (o.job) {
            if (!o.job->done.load(std::memory_order_acquire)) return FUSIONMCP_PENDING;
            const auto& info=o.job->info;
            if (!info.ok) return Write(writer,sink,Error("capture_failed",info.error));
            return Write(writer,sink,Success({{"artifact",{{"name",o.candidate.name},{"path",o.candidate.path},{"mimeType","image/png"},
                {"bytes",info.bytes},{"width",info.width},{"height",info.height},{"bitDepth",info.bitDepth},{"colorType",info.colorType},
                {"sha256",info.sha256}}},{"source","engine_screenshot"},{"completion","file_verified"},{"hdr",false},{"hudHidden",o.hidHud},
                {"colorProcessing","Engine-encoded PNG of the presented SDR frame; HDR values are not preserved"}}));
        }
        const auto files=services->ListScreenshots();
        const FileEntry* newest=nullptr;
        for (const auto& file:files) if (!o.before.contains(file.name)&&(!newest||file.modified>newest->modified)) newest=&file;
        if (!newest) {
            if (now()-o.dispatched>=kCaptureWindow) return Write(writer,sink,Error("capture_failed","Engine wrote no new screenshot file within the capture window"));
            return FUSIONMCP_PENDING;
        }
        if (newest->name!=o.candidate.name||newest->bytes!=o.candidate.bytes||!newest->bytes) {o.candidate=*newest;o.stable=0;return FUSIONMCP_PENDING;}
        if (++o.stable<2) return FUSIONMCP_PENDING;
        // Reading and hashing the file is blocking I/O; keep it off the Game owner.
        o.job=std::make_shared<HashJob>();
        std::thread([job=o.job,path=o.candidate.path] {
            job->info=InspectPng(path);
            job->done.store(true,std::memory_order_release);
        }).detach();
        return FUSIONMCP_PENDING;
    }
    FUSIONMCP_Status Poll(const FUSIONMCP_OperationV1& op,FUSIONMCP_WriteFn writer,void* sink) {
        const std::string operation(op.operation_id,op.operation_id_bytes);
        if (world) if (const auto status=world->Poll(operation,writer,sink)) return *status;
        if (tests) if (const auto status=tests->Poll(operation,writer,sink)) return *status;
        const auto it=operations.find(operation);
        if (it==operations.end()) return FUSIONMCP_INVALID_ARGUMENT;
        const auto status=it->second.kind==Kind::Console?PollConsole(it->second,writer,sink):PollScreenshot(it->second,writer,sink);
        if (status!=FUSIONMCP_PENDING) operations.erase(it);
        return status;
    }
    static State& From(void* context) {return *static_cast<State*>(context);}
    static FUSIONMCP_Status FUSIONMCP_CALL InvokeFn(void* context,const FUSIONMCP_InvocationV1* i,FUSIONMCP_WriteFn writer,void* sink) noexcept {
        try {
            auto& s=From(context);std::scoped_lock lock(s.mutex);
            const std::string command(i->command,i->command_bytes);
            const auto args=ParseBounded(std::string_view(i->arguments_json,i->arguments_bytes),FUSIONMCP_MAX_ARGUMENT_BYTES);
            if (command=="game.readiness") return s.Readiness(*i,writer,sink);
            if (!s.services->ConsoleSupported()) return Write(writer,sink,Error("unavailable","Native Game adapter is not supported on this runtime"));
            if (command=="console.commands") return s.ListCommands(args,writer,sink);
            if (command=="console.execute") return s.Execute(*i,args,writer,sink);
            if (command=="console.run") return s.RunLine(*i,args["line"].get<std::string>(),{},args,writer,sink);
            if (command=="saves.load") return s.LoadSave(*i,args,writer,sink);
            if (command=="capture.screenshot") return s.Screenshot(*i,args,writer,sink);
            if (s.world) if (const auto status=s.world->Invoke(command,*i,args,writer,sink)) return *status;
            if (s.tests) if (const auto status=s.tests->Invoke(command,*i,args,writer,sink)) return *status;
            return Write(writer,sink,Error("unsupported","Unknown built-in command"));
        } catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
    }
    static FUSIONMCP_Status FUSIONMCP_CALL PollFn(void* context,const FUSIONMCP_OperationV1* op,FUSIONMCP_WriteFn writer,void* sink) noexcept {
        try {auto& s=From(context);std::scoped_lock lock(s.mutex);return s.Poll(*op,writer,sink);}
        catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
    }
    // A queued console line or screenshot request cannot be recalled;
    // cancellation only stops observing it (and restores a hidden HUD; world
    // operations release held input). The registry reports that started
    // effects are not undone. A detached hash job owns its own state.
    static FUSIONMCP_Status FUSIONMCP_CALL CancelFn(void* context,const FUSIONMCP_OperationV1* op) noexcept {
        try {
            auto& s=From(context);std::scoped_lock lock(s.mutex);
            const std::string operation(op->operation_id,op->operation_id_bytes);
            if (s.world&&s.world->Cancel(operation)) return FUSIONMCP_OK;
            if (s.tests&&s.tests->Cancel(operation)) return FUSIONMCP_OK;
            if (const auto it=s.operations.find(operation);it!=s.operations.end()) {
                if (it->second.frameId&&s.services->Frames()) s.services->Frames()->Cancel(it->second.frameId);
                if (it->second.capture&&s.services->Capture()) s.services->Capture()->End(it->second.capture);
                s.RestoreHud(it->second);s.operations.erase(it);
            }
            return FUSIONMCP_OK;
        } catch (...) {return FUSIONMCP_INTERNAL_FAILURE;}
    }
};

BuiltinCommands::BuiltinCommands(std::shared_ptr<GameServices> services,RequestBroker::NowFn now):state_(std::make_unique<State>(std::move(now))) {
    auto& s=*state_;
    s.services=std::move(services);
    s.Add("game.readiness","One read-only Game owner sample of menu, player, cell and 3D state. Not a load barrier.",kEmptySchema,0);
    if (s.services->ConsoleSupported()) {
        s.Add("console.commands","Native console command inventory (name, alias, reference flag, declared parameter count, ambiguity).",kListSchema,0);
        s.Add("console.run","Run one console line exactly as typed into the in-game console, on the Game owner: script statements "
            "(set gamehour to 12; if ... ; endif), selected-reference prefixes (player.GetAV Health, 0001A332.GetPos X), "
            "load/save/qqq/bat and every console command. The engine compiles and runs the line before the result is written; "
            "output is everything the console printed for it, without the engine's echo. A load line naming a listed save "
            "returns load.transactionId for fusionmcp.wait_ready. semanticSuccess stays null: read the output.",kRunSchema,
            FUSIONMCP_COMMAND_MUTATING|FUSIONMCP_COMMAND_ASYNC);
        s.Add("console.execute","Typed form of fusionmcp.console.run: one native command {command,target?,arguments?} assembled into a "
            "quoted console line and run the same way, with the same output capture.",kExecuteSchema,
            FUSIONMCP_COMMAND_MUTATING|FUSIONMCP_COMMAND_ASYNC);
        s.Add("saves.load","Validate one exact save from fusionmcp.saves.list and queue its engine load. Completes at dispatch with a "
            "transactionId; poll fusionmcp.wait_ready for the correlated load and stable world readiness.",kLoadSchema,FUSIONMCP_COMMAND_MUTATING);
        s.Add("capture.screenshot","Capture the next presented frame and complete only after the PNG is written and verified "
            "(dimensions and SHA-256). Default method native: a D3D11 readback of the game's swap chain at Present, saved in the "
            "FusionMCP screenshot folder; fails with frames_not_presented when the game presents no frame (minimized, display off). "
            "method engine: the game's ScreenShot command (ScreenShotN.png in the game folder). SDR output; not HDR-preserving. "
            "Optional: hideHud (restored afterwards) and face (turn the player toward a reference first). Fetch the pixels with "
            "fusionmcp.capture.image.",
            kScreenshotSchema,FUSIONMCP_COMMAND_MUTATING|FUSIONMCP_COMMAND_ASYNC);
        if (auto* world=s.services->World()) {
            s.world=std::make_unique<WorldCommands>(*world,*s.services,s.now);
            for (const auto& spec:WorldCommands::Specs()) s.Add(spec.name,spec.description,spec.schema,spec.flags);
            s.tests=std::make_unique<TestCommands>(*world,*s.services,s.markers,s.now);
            for (const auto& spec:TestCommands::Specs()) s.Add(spec.name,spec.description,spec.schema,spec.flags);
        }
    }
    for (std::size_t i=0;i<s.commands.size();++i) {
        s.commands[i].name=s.names[i].data();s.commands[i].name_bytes=static_cast<uint32_t>(s.names[i].size());
        s.commands[i].description=s.descriptions[i].data();s.commands[i].description_bytes=static_cast<uint32_t>(s.descriptions[i].size());
    }
    static constexpr char nameSpace[]="fusionmcp";
    s.provider={FUSIONMCP_ABI_V1,sizeof(FUSIONMCP_ProviderV1),nameSpace,static_cast<uint32_t>(sizeof(nameSpace)-1),0,&s,s.commands.data(),static_cast<uint32_t>(s.commands.size()),0,
        &State::InvokeFn,&State::PollFn,&State::CancelFn};
}
BuiltinCommands::~BuiltinCommands()=default;
const FUSIONMCP_ProviderV1* BuiltinCommands::Provider() const noexcept {return &state_->provider;}
std::vector<std::string> BuiltinCommands::Capabilities() const {
    std::vector<std::string> capabilities{"game_readiness_sample"};
    if (state_->services->ConsoleSupported())
        capabilities.insert(capabilities.end(),{"native_console","console_lines","console_output","named_save_load","save_readiness","native_screenshot"});
    if (state_->world)
        capabilities.insert(capabilities.end(),{"world_inspection","world_spawn","ui_dialogs","camera_control","input_control",
            "condition_waits","animation_probes","screenshot_options","globals","camera_pose","camera_path","frame_stats",
            "frame_timing","log_wait","clean_quit","world_place"});
    return capabilities;
}
std::vector<ProviderRegistry::ControlTool> BuiltinCommands::ControlTools() {
    auto* s=state_.get();
    std::vector<ProviderRegistry::ControlTool> logs{
        {"fusionmcp.logs.list","Readable Papyrus and F4SE plugin logs (file system only): name, bytes, newest first.",Schema(kEmptySchema),false,
            [s](const Json&,bool) {
                auto files=s->services->ListLogs();
                std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.modified>b.modified;});
                Json logs=Json::array();
                for (const auto& f:files) logs.push_back({{"name",PrintableText(f.name)},{"bytes",f.bytes}});
                return Success({{"logs",std::move(logs)}});
            }},
        {"fusionmcp.logs.read","Read a log by name ('*' picks the newest match, e.g. F4SE/F4FS*.log): from offset (a previous "
            "nextOffset), from since (a fusionmcp.logs.mark marker or its stored id: only lines appended after the mark), or its last "
            "maxBytes (default 16384). contains keeps matching lines only. Returns marker for the next read. Text is sanitised to "
            "printable ASCII.",Schema(kLogReadSchema),false,
            [s](const Json& args,bool) {return ReadLog(s->services->ListLogs(),s->markers,args);}},
        {"fusionmcp.logs.mark","Mark the current end of a log ('*' picks the newest match). Returns marker <log>#<offset>; with id it is "
            "also stored under that id, so a later fusionmcp.logs.read or fusionmcp.logs.wait (even a later batch step) passes since=id and "
            "sees only lines appended after the mark.",Schema(kLogMarkSchema),false,
            [s](const Json& args,bool) {
                const auto found=ResolveLog(s->services->ListLogs(),args["name"].get<std::string>());
                if (!found) return Error("not_found","No listed log has this name");
                std::error_code error;
                const auto size=std::filesystem::file_size(std::filesystem::path(found->path),error);
                if (error) return Error("unavailable","Log size cannot be read");
                const LogMarker marker{found->name,size};
                Json result={{"log",PrintableText(found->name)},{"offset",size},{"marker",FormatMarker(marker)}};
                if (args.contains("id")) {s->markers.Store(args["id"].get<std::string>(),marker);result["id"]=args["id"];}
                return Success(std::move(result));
            }},
    };
    if (!state_->services->ConsoleSupported()) return logs;
    std::vector<ProviderRegistry::ControlTool> tools={
        {"fusionmcp.saves.list","Exact save names in the engine save directory, newest first (file system only).",
            Schema(R"({"type":"object","properties":{"limit":{"type":"integer","minimum":1,"maximum":200}},"additionalProperties":false})"),false,
            [s](const Json& args,bool) {
                auto files=s->services->ListSaves();
                std::erase_if(files,[](const FileEntry& f){return f.name.size()<=4||f.name.substr(f.name.size()-4)!=".fos";});
                std::sort(files.begin(),files.end(),[](const auto& a,const auto& b){return a.modified>b.modified;});
                Json saves=Json::array();
                const auto limit=args.value("limit",50u);
                for (const auto& f:files) {
                    if (saves.size()>=limit) break;
                    saves.push_back({{"save",PrintableText(f.name.substr(0,f.name.size()-4))},{"bytes",f.bytes}});
                }
                return Success({{"saves",std::move(saves)},{"total",files.size()},{"truncated",files.size()>limit}});
            }},
        {"fusionmcp.wait_ready","Nonblocking load barrier: state of a fusionmcp.saves.load transaction (default: most recent). ready=true only after "
            "the correlated target load succeeded and the world was stably ready on the Game owner. Poll until ready or terminal.",
            Schema(R"({"type":"object","properties":{"transactionId":{"type":"integer","minimum":1}},"additionalProperties":false})"),false,
            [s](const Json& args,bool) {
                const auto snapshot=s->saves.Snapshot(args.value("transactionId",0ull));
                return snapshot.is_null()?Error("not_found","No save load transaction is retained"):Success(snapshot);
            }},
        {"fusionmcp.capture.image","Return a screenshot (default: newest native or engine one) as an MCP image block: a JPEG scaled "
            "to maxWidth (default 1280), with quality and size lowered as needed to fit the response. capture=true first takes a "
            "fresh native frame (no Game owner needed; fails with frames_not_presented when the game presents no frame) and "
            "returns it with its path, bytes and sha256. The full PNG stays at path.",
            Schema(R"({"type":"object","properties":{"name":{"type":"string","minLength":1,"maxLength":64},)"
                R"("capture":{"type":"boolean"},)"
                R"("maxWidth":{"type":"integer","minimum":64,"maximum":1920}},"additionalProperties":false})"),false,
            [s](const Json& args,bool) {
                auto* frames=s->services->Frames();
                Json captured;
                FileEntry chosen;
                if (args.value("capture",false)) {
                    if (args.contains("name")) return Error("invalid_arguments","Pass either name or capture, not both");
                    if (!frames) return Error("unavailable",NativeUnavailable(frames));
                    const auto admitted=frames->Request();
                    if (!admitted.code.empty()) return Error(admitted.code,admitted.message);
                    const auto limit=std::chrono::steady_clock::now()+kNativeImageWait;
                    auto status=frames->Poll(admitted.id);
                    for (;status.state==FrameCapture::State::Pending;status=frames->Poll(admitted.id)) {
                        if (std::chrono::steady_clock::now()>=limit) {
                            frames->Cancel(admitted.id);
                            return Error("capture_failed","Native capture did not finish within the wait bound");
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(5));
                    }
                    if (status.state==FrameCapture::State::Failed) return Error(status.code,status.message);
                    captured=NativeArtifact(status);
                    captured["presentCount"]=status.presentCount;
                    chosen={status.name,status.path,status.artifact.bytes,0};
                } else {
                    auto files=s->services->ListScreenshots();
                    if (frames) for (auto& f:frames->Files()) files.push_back(std::move(f));
                    const FileEntry* found=nullptr;
                    const auto wanted=args.contains("name")?Lower(args["name"].get<std::string>()):std::string();
                    for (const auto& f:files) {
                        if (!wanted.empty()?Lower(f.name)==wanted:(!found||f.modified>found->modified)) found=&f;
                    }
                    if (!found) return Error("not_found",wanted.empty()?"No screenshots exist":"No screenshot has this name");
                    chosen=*found;
                }
                auto preview=EncodePreview(chosen.path,args.value("maxWidth",1280u),kMaxImageBase64Bytes);
                if (!preview.ok) return Error("capture_failed",preview.error);
                Json result={{"name",PrintableText(chosen.name)},{"path",PrintableText(chosen.path)},
                    {"sourceWidth",preview.sourceWidth},{"sourceHeight",preview.sourceHeight},
                    {"width",preview.width},{"height",preview.height},{"quality",preview.quality},
                    {"image",{{"mimeType","image/jpeg"},{"data",std::move(preview.base64)}}}};
                if (!captured.is_null()) {
                    result["bytes"]=captured["bytes"];result["sha256"]=captured["sha256"];
                    result["presentCount"]=captured["presentCount"];result["source"]="native_present_readback";
                }
                return Success(std::move(result));
            }},
    };
    for (auto& tool:logs) tools.push_back(std::move(tool));
    return tools;
}
bool BuiltinCommands::RegisterWith(ProviderRegistry& core) {
    if (core.RegisterBuiltin(Provider(),Capabilities())!=FUSIONMCP_OK) return false;
    for (auto& tool:ControlTools()) if (!core.RegisterControl(std::move(tool))) return false;
    return true;
}
SaveLoadCoordinator& BuiltinCommands::Saves() noexcept {return state_->saves;}
void BuiltinCommands::SampleOnGame(EpochId epoch) {
    if (state_->saves.NeedsSamples()) state_->saves.Sample(state_->services->SampleReadiness(),epoch);
    if (state_->tests) {std::scoped_lock lock(state_->mutex);state_->tests->SampleOnGame();}
}
bool BuiltinCommands::NeedsGameWake() const {
    return state_->saves.NeedsSamples()||(state_->tests&&state_->tests->NeedsGameWake());
}
void BuiltinCommands::SetF4fs(std::function<Json(const std::string&,const Json&)> caller) {
    if (state_->tests) state_->tests->SetF4fs(std::move(caller));
}
}
