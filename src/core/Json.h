#pragma once
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <string_view>

namespace fusionmcp {
using Json = nlohmann::json;
inline Json ParseBounded(std::string_view bytes, std::size_t limit = 262144, unsigned depthLimit=32) {
    if (bytes.size() > limit) throw std::invalid_argument("JSON byte limit");
    std::vector<std::set<std::string>> keys;
    return Json::parse(bytes.begin(), bytes.end(), [&](int depth, Json::parse_event_t event, Json& value) {
        if (depth > static_cast<int>(depthLimit)) throw std::invalid_argument("JSON depth");
        if (event == Json::parse_event_t::object_start) keys.emplace_back();
        if (event == Json::parse_event_t::key && (keys.empty() || !keys.back().insert(value.get<std::string>()).second))
            throw std::invalid_argument("duplicate JSON key");
        if (event == Json::parse_event_t::object_end) keys.pop_back();
        return true;
    });
}
bool SchemaSupported(const Json& schema, unsigned depth = 0);
bool MatchesSchema(const Json& value, const Json& schema, unsigned depth = 0);
inline Json Error(std::string_view code, std::string_view message) {
    return {{"ok",false},{"error",{{"code",code},{"message",message}}}};
}
inline Json Success(Json result) { return {{"ok",true},{"result",std::move(result)}}; }
inline bool Envelope(const Json& value) {
    if (!value.is_object() || !value.contains("ok") || !value["ok"].is_boolean()) return false;
    if (value["ok"].get<bool>()) return value.contains("result");
    return value.contains("error") && value["error"].is_object() &&
        value["error"].contains("code") && value["error"]["code"].is_string() &&
        value["error"].contains("message") && value["error"]["message"].is_string();
}
}
