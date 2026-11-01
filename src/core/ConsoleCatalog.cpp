#include "ConsoleCatalog.h"
#include <algorithm>
#include <charconv>
#include <cmath>

namespace fusionmcp {
namespace {
std::string Fold(std::string_view text) {
    std::string folded(text);
    for (char& c:folded) if (c>='A'&&c<='Z') c=static_cast<char>(c+('a'-'A'));
    return folded;
}
bool Matches(const ConsoleRow& row,std::string_view folded) {
    return Fold(row.name)==folded||(!row.alias.empty()&&Fold(row.alias)==folded);
}
bool CommandToken(std::string_view text) {
    return !text.empty()&&text.size()<=64&&std::all_of(text.begin(),text.end(),[](unsigned char c) {
        return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
    });
}
bool Hex8(std::string_view text) {
    return text.size()==8&&std::all_of(text.begin(),text.end(),[](unsigned char c) {
        return (c>='0'&&c<='9')||(c>='a'&&c<='f')||(c>='A'&&c<='F');
    });
}
// One typed argument stays one token of one statement: the console grammar has
// no escape for '"', and ProcessCommand splits statements on ';'.
bool SafeString(std::string_view text) {
    return !text.empty()&&text.size()<=kMaxConsoleStringBytes&&std::all_of(text.begin(),text.end(),[](unsigned char c) {
        return c>=32&&c!=127&&c!='"'&&c!=';';
    });
}
bool Space(char c) {return c==' '||c=='\t';}
std::string Truncated(const std::string& text,std::size_t limit) {return text.size()<=limit?text:text.substr(0,limit);}
}

ConsoleCatalog::ConsoleCatalog(std::vector<ConsoleRow> rows):rows_(std::move(rows)) {
    std::erase_if(rows_,[](const ConsoleRow& row){return row.name.empty();});
}
std::vector<const ConsoleRow*> ConsoleCatalog::Candidates(std::string_view token) const {
    std::vector<const ConsoleRow*> rows;
    const auto folded=Fold(token);
    if (!folded.empty()) for (const auto& row:rows_) if (Matches(row,folded)) rows.push_back(&row);
    return rows;
}
ConsoleMatch ConsoleCatalog::Resolve(std::string_view token) const {
    ConsoleMatch match;
    const auto folded=Fold(token);
    if (folded.empty()) return match;
    for (const auto& row:rows_) if (Matches(row,folded)) {++match.candidates;match.row=&row;}
    if (match.candidates>1) {match.status=ConsoleResolution::Ambiguous;match.row=nullptr;}
    else if (match.candidates==1) match.status=ConsoleResolution::Unique;
    return match;
}
Json ConsoleCatalog::List(std::string_view filter,std::size_t offset,std::size_t limit) const {
    const auto folded=Fold(filter);
    Json commands=Json::array();
    std::size_t matched=0;
    for (const auto& row:rows_) {
        if (!folded.empty()&&Fold(row.name).find(folded)==std::string::npos&&Fold(row.alias).find(folded)==std::string::npos) continue;
        if (matched++<offset||commands.size()>=limit) continue;
        commands.push_back({{"name",row.name},{"alias",row.alias},{"help",Truncated(row.help,256)},{"table",row.table},
            {"referenceFunction",row.referenceFunction},{"paramCount",row.paramCount},
            {"ambiguous",Resolve(row.name).status==ConsoleResolution::Ambiguous},
            {"lifecycle",IsLifecycleCommand(row)}});
    }
    return {{"commands",std::move(commands)},{"matched",matched},{"offset",offset},{"total",rows_.size()},
        {"truncated",matched>offset+limit}};
}
namespace {
bool Named(const ConsoleRow& row,std::initializer_list<std::string_view> names) {
    const auto has=[&](const std::string& text){return std::find(names.begin(),names.end(),Fold(text))!=names.end();};
    return has(row.name)||(!row.alias.empty()&&has(row.alias));
}
}
bool IsLifecycleCommand(const ConsoleRow& row) {
    return Named(row,{"loadgame","load","save","savegame","quitgame","qqq","bat","runconsolebatch"});
}
bool IsLoadCommand(const ConsoleRow& row) {return Named(row,{"loadgame","load"});}
bool IsBatchCommand(const ConsoleRow& row) {return Named(row,{"bat","runconsolebatch"});}
std::optional<ConsoleRow> MergeRows(const std::vector<const ConsoleRow*>& rows) {
    if (rows.empty()) return std::nullopt;
    ConsoleRow merged=*rows.front();
    for (const auto* row:rows) {
        if (Fold(row->name)!=Fold(merged.name)) return std::nullopt;
        merged.paramCount=std::max(merged.paramCount,row->paramCount);
        merged.referenceFunction=merged.referenceFunction&&row->referenceFunction;
    }
    return merged;
}
ConsoleLine AssembleConsoleLine(const ConsoleRow& row,const Json& arguments) {
    ConsoleLine line;
    const auto fail=[&](std::string code,std::string message){line.text.clear();line.errorCode=std::move(code);line.errorMessage=std::move(message);return line;};
    if (!CommandToken(row.name)) return fail("unsupported_console_command","Native command name is not a plain console token");
    const auto target=arguments.value("target",std::string());
    // The native flag means a reference is required, not that one is refused:
    // e.g. PlaceAtMe is unflagged yet is normally run as player.PlaceAtMe.
    if (row.referenceFunction&&target.empty()) return fail("target_required","Reference command requires an explicit target");
    if (!target.empty()) {
        if (target=="player") line.text="player.";
        else if (Hex8(target)) {line.text=target;for (char& c:line.text) if (c>='a'&&c<='f') c=static_cast<char>(c-('a'-'A'));line.text+='.';}
        else return fail("invalid_target","Target must be \"player\" or an 8-digit hexadecimal runtime FormID");
    }
    line.text+=row.name;
    const auto values=arguments.value("arguments",Json::array());
    if (values.size()>kMaxConsoleArguments||values.size()>row.paramCount)
        return fail("too_many_arguments","More arguments than the native command declares");
    for (const auto& value:values) {
        line.text+=' ';
        if (value.is_string()) {
            const auto& text=value.get_ref<const std::string&>();
            if (!SafeString(text)) return fail("invalid_arguments","String arguments are 1..256 characters without control characters, '\"' or ';' (use fusionmcp.console.run for full lines)");
            if (text.find_first_of(" \t")!=std::string::npos) line.text+='"'+text+'"'; else line.text+=text;
        } else if (value.is_number_integer()) {
            line.text+=value.dump();
        } else if (value.is_number()) {
            const auto number=value.get<double>();
            if (!std::isfinite(number)||std::fabs(number)>=1e9) return fail("invalid_arguments","Numbers must be finite and below 1e9 in magnitude");
            char buffer[64];
            const auto result=std::to_chars(buffer,buffer+sizeof(buffer),number,std::chars_format::fixed,6);
            std::string text(buffer,result.ptr);
            while (text.back()=='0') text.pop_back();
            if (text.back()=='.') text.pop_back();
            line.text+=text;
        } else return fail("invalid_arguments","Arguments are strings or numbers");
    }
    if (line.text.size()>kMaxConsoleLineBytes) return fail("invalid_arguments","Assembled console line exceeds the native buffer bound");
    return line;
}
ConsoleLine ValidateConsoleLine(std::string_view text) {
    ConsoleLine line;
    if (text.find_first_not_of(" \t")==std::string_view::npos) {line.errorCode="invalid_line";line.errorMessage="Line is empty";return line;}
    if (text.size()>kMaxConsoleLineBytes) {
        line.errorCode="invalid_line";line.errorMessage="Line exceeds "+std::to_string(kMaxConsoleLineBytes)+" bytes (engine console buffers are 1024)";
        return line;
    }
    if (std::any_of(text.begin(),text.end(),[](unsigned char c){return (c<32&&c!='\t')||c==127;})) {
        line.errorCode="invalid_line";line.errorMessage="Control characters cannot be typed into the console; separate statements with ';'";
        return line;
    }
    line.text=std::string(text);
    return line;
}
ConsoleStatement ParseConsoleStatement(std::string_view line) {
    ConsoleStatement statement;
    statement.single=line.find(';')==std::string_view::npos;
    if (const auto end=line.find(';');end!=std::string_view::npos) line=line.substr(0,end);
    std::size_t at=0;
    while (at<line.size()&&Space(line[at])) ++at;
    auto stop=at;
    while (stop<line.size()&&!Space(line[stop])) ++stop;
    auto head=line.substr(at,stop-at);
    // "player.GetPos" / "0001A332.GetPos": the selected-reference prefix.
    if (const auto dot=head.find('.');dot!=std::string_view::npos&&dot>0&&dot+1<head.size()&&head.front()!='"') {
        statement.target=std::string(head.substr(0,dot));head.remove_prefix(dot+1);
    }
    statement.command=std::string(head);
    auto rest=line.substr(stop);
    while (!rest.empty()&&Space(rest.front())) rest.remove_prefix(1);
    while (!rest.empty()&&Space(rest.back())) rest.remove_suffix(1);
    if (rest.size()>=2&&rest.front()=='"'&&rest.back()=='"') {rest.remove_prefix(1);rest.remove_suffix(1);}
    statement.argument=std::string(rest);
    return statement;
}
std::string StripConsoleEcho(std::string output,std::string_view line) {
    for (const auto& echo:{std::string(line)+"\n","Command: "+std::string(line)+"\n"})
        if (output.starts_with(echo)) output.erase(0,echo.size());
    return output;
}
}
