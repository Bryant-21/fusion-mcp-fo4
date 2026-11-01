#include "Json.h"
#include <cmath>
namespace fusionmcp {
bool SchemaSupported(const Json& s, unsigned depth) {
    if (depth > 16 || !s.is_object()) return false;
    static const std::set<std::string> allowed = {"type","properties","required","additionalProperties","enum",
        "minimum","maximum","minLength","maxLength","description","default","title","oneOf","items","minItems","maxItems"};
    for (auto i=s.begin();i!=s.end();++i) if (!allowed.contains(i.key())) return false;
    if (s.contains("type") && (!s["type"].is_string() ||
        !std::set<std::string>{"object","array","string","integer","number","boolean","null"}.contains(s["type"].get<std::string>()))) return false;
    for (const auto* key : {"minLength","maxLength","minItems","maxItems"})
        if (s.contains(key) && (!s[key].is_number_integer() || s[key].get<double>()<0)) return false;
    for (const auto* key : {"minimum","maximum"}) if (s.contains(key) && !s[key].is_number()) return false;
    for (const auto* key : {"description","title"}) if (s.contains(key) && !s[key].is_string()) return false;
    if (s.contains("additionalProperties") && !s["additionalProperties"].is_boolean()) return false;
    if (s.contains("enum") && (!s["enum"].is_array() || s["enum"].empty() || s["enum"].size()>128)) return false;
    if (s.contains("oneOf")) {
        if (!s["oneOf"].is_array() || s["oneOf"].empty() || s["oneOf"].size()>8) return false;
        for (const auto& branch:s["oneOf"]) if (!SchemaSupported(branch,depth+1)) return false;
    }
    if (s.contains("properties")) {
        if (!s["properties"].is_object() || s["properties"].size()>128) return false;
        for (const auto& p:s["properties"]) if (!SchemaSupported(p,depth+1)) return false;
    }
    if (s.contains("required")) {
        if (!s["required"].is_array() || s["required"].size()>128) return false;
        std::set<std::string> names;
        for (const auto& key:s["required"]) if (!key.is_string() || !names.insert(key.get<std::string>()).second ||
            !s.contains("properties") || !s["properties"].contains(key.get<std::string>())) return false;
    }
    return !s.contains("items") || SchemaSupported(s["items"],depth+1);
}
bool MatchesSchema(const Json& v,const Json& s,unsigned depth) {
    if (!SchemaSupported(s,depth)) return false;
    if (s.contains("oneOf")) {
        unsigned matches=0;
        for (const auto& branch:s["oneOf"]) if (MatchesSchema(v,branch,depth+1)) ++matches;
        if (matches!=1) return false;
    }
    if (s.contains("type")) {
        const auto t=s["type"].get<std::string>();
        if ((t=="object"&&!v.is_object())||(t=="array"&&!v.is_array())||(t=="string"&&!v.is_string())||
            (t=="integer"&&!v.is_number_integer())||(t=="number"&&!v.is_number())||
            (t=="boolean"&&!v.is_boolean())||(t=="null"&&!v.is_null())) return false;
    }
    if (s.contains("enum") && std::find(s["enum"].begin(),s["enum"].end(),v)==s["enum"].end()) return false;
    if (v.is_number()) {
        const auto n=v.get<double>();
        if (!std::isfinite(n)||(s.contains("minimum")&&n<s["minimum"].get<double>())||
            (s.contains("maximum")&&n>s["maximum"].get<double>())) return false;
    }
    if (v.is_string()) {
        const auto n=v.get_ref<const std::string&>().size();
        if ((s.contains("minLength")&&n<s["minLength"].get<std::size_t>())||
            (s.contains("maxLength")&&n>s["maxLength"].get<std::size_t>())) return false;
    }
    if (v.is_array()) {
        if ((s.contains("minItems")&&v.size()<s["minItems"].get<std::size_t>())||
            (s.contains("maxItems")&&v.size()>s["maxItems"].get<std::size_t>())) return false;
        if (!s.contains("items")) return v.empty();
        for (const auto& item:v) if (!MatchesSchema(item,s["items"],depth+1)) return false;
    }
    if (v.is_object()) {
        if (s.contains("required")) for (const auto& key:s["required"]) if (!v.contains(key.get<std::string>())) return false;
        const auto properties=s.value("properties",Json::object());
        for (auto i=v.begin();i!=v.end();++i) {
            if (properties.contains(i.key())) { if (!MatchesSchema(i.value(),properties[i.key()],depth+1)) return false; }
            else if (!s.value("additionalProperties",false)) return false;
        }
    }
    return true;
}
}
