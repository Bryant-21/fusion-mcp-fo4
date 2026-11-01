#include "Protocol.h"
#include "core/Json.h"
#include "core/Limits.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <charconv>
#include <cctype>
#include <limits>
#include <set>
#include <stdexcept>
#include <string_view>

namespace fusionmcp::transport
{
    namespace
    {
        using Json = nlohmann::json;
        std::string Lower(std::string text)
        {
            for (char& c : text) if (c >= 'A' && c <= 'Z') c = char(c + ('a' - 'A'));
            return text;
        }
        bool Printable(std::string_view text)
        {
            return std::all_of(text.begin(), text.end(), [](unsigned char c) { return c >= 32 && c <= 126; });
        }
        bool HeaderName(std::string_view text)
        {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                    (c >= '0' && c <= '9') || c == '-';
            });
        }
        std::string Trim(std::string_view text)
        {
            while (!text.empty() && (text.front() == ' ' || text.front() == '\t')) text.remove_prefix(1);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) text.remove_suffix(1);
            return std::string(text);
        }
        const std::string* Header(const Request& request, std::string_view name)
        {
            for (const auto& [key, value] : request.headers) if (Lower(key) == name) return &value;
            return nullptr;
        }
        Response Error(unsigned status, std::string_view code)
        {
            return {status, "application/json", Json{{"error", code}}.dump(), {}};
        }
        Response RpcError(const Json& id, int code, std::string_view message)
        {
            return {200, "application/json", Json{{"jsonrpc", "2.0"}, {"id", id},
                {"error", {{"code", code}, {"message", message}}}}.dump(), {}};
        }
        Response RpcResult(const Json& id, const Json& result)
        {
            return {200, "application/json", Json{{"jsonrpc", "2.0"}, {"id", id}, {"result", result}}.dump(), {}};
        }
        Json ParseJson(const std::string& text)
        {
            std::vector<std::set<std::string>> keys;
            return Json::parse(text, [&](int depth, Json::parse_event_t event, Json& parsed) {
                if (depth > 32) throw std::invalid_argument("JSON depth");
                if (event == Json::parse_event_t::object_start) keys.emplace_back();
                if (event == Json::parse_event_t::key &&
                    (keys.empty() || !keys.back().insert(parsed.get<std::string>()).second))
                    throw std::invalid_argument("duplicate JSON key");
                if (event == Json::parse_event_t::object_end) keys.pop_back();
                return true;
            });
        }
        std::shared_ptr<const Backend> Compatible(const ProviderResolver& resolve)
        {
            auto backend = resolve ? resolve() : nullptr;
            return backend && backend->describe && backend->invoke && backend->lookup ? backend : nullptr;
        }
        bool ValidDescriptor(const Json& tool) {
            return tool.is_object() && tool.contains("name") && tool["name"].is_string() &&
                tool.contains("inputSchema") && fusionmcp::SchemaSupported(tool["inputSchema"]) &&
                tool.contains("_meta") && tool["_meta"].is_object() &&
                tool["_meta"].contains("fusionmcp.mutating") && tool["_meta"]["fusionmcp.mutating"].is_boolean();
        }
        Json Tools(const std::shared_ptr<const Backend>& backend)
        {
            if (!backend) return Json::array();
            auto tools = fusionmcp::ParseBounded(backend->describe(),kMaxCatalogBytes,kMaxCatalogDepth);
            if (!tools.is_array() || tools.size() > 128) throw std::runtime_error("catalog descriptors");
            std::set<std::string> names;
            for (const auto& tool : tools) {
                if (!ValidDescriptor(tool) ||
                    !names.insert(tool["name"].get<std::string>()).second) throw std::runtime_error("catalog descriptors");
            }
            return tools;
        }
        Json Lookup(const std::shared_ptr<const Backend>& backend,const std::string& name) {
            if (!backend) return Json();
            auto descriptor=fusionmcp::ParseBounded(backend->lookup(name),kMaxDescriptorBytes,kMaxCatalogDepth);
            if (descriptor.is_null()) return descriptor;
            if (!ValidDescriptor(descriptor) || descriptor["name"]!=name) throw std::runtime_error("command descriptor");
            return descriptor;
        }
        Json Invoke(const std::shared_ptr<const Backend>& backend, bool allowMutations, const std::string& tool, const Json& args,const Json& descriptor)
        {
            if (!backend) return fusionmcp::Error("unavailable", "Standalone core unavailable");
            if (descriptor.is_null()) return fusionmcp::Error("unsupported", "Command is not registered");
            if (descriptor["_meta"]["fusionmcp.mutating"].get<bool>() && !allowMutations)
                return fusionmcp::Error("read_only", "Host mutations are disabled");
            if (!fusionmcp::MatchesSchema(args, descriptor["inputSchema"]))
                return fusionmcp::Error("invalid_arguments", "Arguments do not match the command schema");
            const auto serialized = args.dump();
            if (serialized.size() > kMaxBodyBytes || tool.size() > 128)
                return fusionmcp::Error("invalid_arguments", "Argument size limit");
            auto result = fusionmcp::ParseBounded(backend->invoke(tool, serialized, allowMutations),kMaxCoreEnvelopeBytes,kMaxCoreEnvelopeDepth);
            if (!fusionmcp::Envelope(result)) return fusionmcp::Error("core_error", "Invalid result envelope");
            return result;
        }
        // A successful result carrying image {mimeType, data} is returned as an MCP image
        // block; the base64 payload is not repeated in the text or structured content.
        Json McpResult(Json envelope)
        {
            Json content = Json::array();
            if (envelope.value("ok", false) && envelope.contains("result") && envelope["result"].is_object()) {
                auto& result = envelope["result"];
                const auto image = result.find("image");
                if (image != result.end() && image->is_object() && (*image)["data"].is_string() && (*image)["mimeType"].is_string()) {
                    content.push_back({{"type", "image"}, {"data", (*image)["data"]}, {"mimeType", (*image)["mimeType"]}});
                    result.erase(image);
                }
            }
            content.push_back({{"type", "text"}, {"text", envelope.dump()}});
            const bool isError = !envelope.value("ok", false);
            return {{"content", std::move(content)}, {"structuredContent", std::move(envelope)}, {"isError", isError}};
        }
        bool SupportedVersion(std::string_view version)
        {
            return version == kProtocolVersion || version == "2025-06-18" || version == "2025-03-26";
        }
        bool AcceptsMcp(const std::string& header)
        {
            bool json = false, events = false;
            std::string_view remaining(header);
            while (!remaining.empty()) {
                const auto comma = remaining.find(',');
                const auto item = remaining.substr(0, comma);
                const auto semicolon = item.find(';');
                const auto media = Lower(Trim(item.substr(0, semicolon)));
                double quality = 1.0;
                bool seenQuality = false;
                auto parameters = semicolon == item.npos ? std::string_view{} : item.substr(semicolon + 1);
                while (!parameters.empty()) {
                    const auto end = parameters.find(';');
                    const auto parameter = Trim(parameters.substr(0, end));
                    if (!parameter.starts_with("q=") || seenQuality) return false;
                    seenQuality = true;
                    const auto parsed = std::from_chars(parameter.data() + 2, parameter.data() + parameter.size(), quality);
                    if (parsed.ec != std::errc{} || parsed.ptr != parameter.data() + parameter.size() ||
                        !(quality >= 0 && quality <= 1)) return false;
                    parameters = end == parameters.npos ? std::string_view{} : parameters.substr(end + 1);
                }
                if (quality > 0 && media == "application/json") json = true;
                if (quality > 0 && media == "text/event-stream") events = true;
                remaining = comma == remaining.npos ? std::string_view{} : remaining.substr(comma + 1);
            }
            return json && events;
        }
    }

    bool ParseHead(const std::string& head, Request& request, std::size_t& bodyBytes) noexcept
    {
        try {
            if (head.size() > kMaxHeaderBytes || !head.ends_with("\r\n\r\n")) return false;
            Request parsed;
            const auto firstEnd = head.find("\r\n");
            const auto first = std::string_view(head).substr(0, firstEnd);
            const auto space = first.find(' ');
            const auto second = space == first.npos ? first.npos : first.find(' ', space + 1);
            if (space == first.npos || second == first.npos || first.substr(second + 1) != "HTTP/1.1") return false;
            parsed.method = first.substr(0, space);
            parsed.target = first.substr(space + 1, second - space - 1);
            if (!Printable(parsed.method) || !Printable(parsed.target) || parsed.target.empty() ||
                parsed.target.front() != '/' || parsed.method.empty() || parsed.method.size() > 16 || parsed.target.size() > 256) return false;
            std::set<std::string> seen;
            auto start = firstEnd + 2;
            while (start < head.size() - 2) {
                const auto end = head.find("\r\n", start);
                if (end == head.npos || end == start) return false;
                const auto line = std::string_view(head).substr(start, end - start);
                const auto colon = line.find(':');
                if (colon == line.npos || !HeaderName(line.substr(0, colon))) return false;
                const auto name = Lower(std::string(line.substr(0, colon)));
                const auto value = Trim(line.substr(colon + 1));
                if (!Printable(value) || !seen.insert(name).second || parsed.headers.size() >= 64) return false;
                parsed.headers.emplace_back(name, value);
                start = end + 2;
            }
            if (Header(parsed, "transfer-encoding") || Header(parsed, "expect")) return false;
            bodyBytes = 0;
            if (const auto* length = Header(parsed, "content-length")) {
                if (length->empty()) return false;
                const auto result = std::from_chars(length->data(), length->data() + length->size(), bodyBytes);
                if (result.ec != std::errc{} || result.ptr != length->data() + length->size() || bodyBytes > kMaxBodyBytes) return false;
            } else if (parsed.method == "POST") return false;
            if (parsed.method != "POST" && bodyBytes != 0) return false;
            request = std::move(parsed);
            return true;
        } catch (...) { return false; }
    }
    std::string EncodeResponse(const Response& response)
    {
        const char* reason = response.status == 200 ? "OK" : response.status == 202 ? "Accepted" :
            response.status == 400 ? "Bad Request" : response.status == 401 ? "Unauthorized" :
            response.status == 403 ? "Forbidden" : response.status == 404 ? "Not Found" :
            response.status == 405 ? "Method Not Allowed" : response.status == 413 ? "Payload Too Large" :
            response.status == 415 ? "Unsupported Media Type" :
            response.status == 503 ? "Service Unavailable" : "Internal Server Error";
        std::string text = "HTTP/1.1 " + std::to_string(response.status) + " " + reason + "\r\n";
        text += "Content-Length: " + std::to_string(response.body.size()) + "\r\n";
        text += "Content-Type: " + response.contentType + "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n";
        for (const auto& [key, value] : response.headers) text += key + ": " + value + "\r\n";
        return text + "\r\n" + response.body;
    }
    Dispatcher::Dispatcher(Config config, ProviderResolver resolver) : config_(std::move(config)), resolver_(std::move(resolver)) {}
    void Dispatcher::Tick() const noexcept
    {
        try {
            if (!config_.enabled) return;
            const auto backend=Compatible(resolver_);
            if (backend && backend->tick) backend->tick();
        } catch (...) {}
    }
    Response Dispatcher::Handle(const Request& request) const noexcept
    {
        try {
            auto response = HandleChecked(request);
            if (response.body.size() > kMaxResponseBytes) return Error(500, "response_limit");
            return response;
        } catch (...) {
            // No provider exception, malformed UTF-8, allocation or serialization
            // failure crosses the host boundary. No request/token is reflected.
            try { return Error(500, "host_failure"); }
            catch (...) { return {500, "application/json", {}, {}}; }
        }
    }
    Response Dispatcher::HandleChecked(const Request& request) const
    {
        if (!config_.enabled) return Error(503, "disabled");
        if (request.body.size() > kMaxBodyBytes) return Error(413, "body_limit");
        std::size_t headerBytes = request.method.size() + request.target.size() + 16;
        std::set<std::string> headers;
        for (const auto& [name, value] : request.headers) {
            headerBytes += name.size() + value.size() + 4;
            if (!HeaderName(name) || !Printable(value) || !headers.insert(Lower(name)).second || headers.size() > 64) return Error(400, "invalid_headers");
        }
        if (headerBytes > kMaxHeaderBytes) return Error(413, "header_limit");
        const auto port = std::to_string(config_.port);
        const auto loopback = "127.0.0.1:" + port;
        const auto localhost = "localhost:" + port;
        // Secret-free browser guard. The listener binds 127.0.0.1 only; the Host
        // check defeats DNS rebinding, browsers send Origin on cross-site POSTs,
        // and a JSON Content-Type cannot be sent cross-origin without a CORS
        // preflight this server never answers.
        const auto* host = Header(request, "host");
        if (!host || (*host != loopback && *host != localhost)) return Error(403, "invalid_host");
        const auto* origin = Header(request, "origin");
        if (origin && *origin != "null") return Error(403, "invalid_origin");
        if (request.method == "POST") {
            const auto* contentType = Header(request, "content-type");
            const auto media = contentType ? Lower(*contentType) : std::string();
            if (media != "application/json" && media != "application/json; charset=utf-8") return Error(415, "content_type");
        }
        if (request.target == "/mcp" && request.method == "GET") {
            auto response = Error(405, "no_sse");
            response.headers.emplace_back("Allow", "POST");
            return response;
        }
        if (request.target == "/api/tools" && request.method == "GET")
            return {200, "application/json", Json{{"tools", Tools(Compatible(resolver_))}}.dump(), {}};
        if (request.method != "POST" || (request.target != "/mcp" && !request.target.starts_with("/api/tool/")))
            return Error(404, "not_found");
        Json body;
        try { body = ParseJson(request.body); }
        catch (...) { return request.target == "/mcp" ? RpcError(nullptr, -32700, "Parse error") : Error(400, "invalid_json"); }
        if (request.target.starts_with("/api/tool/")) {
            if (!body.is_object()) return Error(400, "invalid_arguments");
            const auto tool = request.target.substr(10);
            if (tool.empty() || tool.size() > 128) return Error(404, "unknown_tool");
            const auto backend=Compatible(resolver_);
            const auto descriptor=Lookup(backend,tool);
            if (descriptor.is_null()) return Error(404,"unknown_tool");
            const auto envelope = Invoke(backend, config_.allowMutations, tool, body,descriptor);
            return {200, "application/json", envelope.dump(), {}};
        }
        const auto* accept = Header(request, "accept");
        if (!accept || !AcceptsMcp(*accept)) return Error(400, "accept_type");
        if (!body.is_object() || body.value("jsonrpc", Json()) != "2.0" ||
            !body.contains("method") || !body["method"].is_string()) return RpcError(nullptr, -32600, "Invalid Request");
        const auto method = body["method"].get<std::string>();
        const auto id = body.value("id", Json());
        if ((body.contains("id") && !id.is_string() && !id.is_number_integer()) ||
            (id.is_string() && id.get_ref<const std::string&>().size() > 128)) return RpcError(nullptr, -32600, "Invalid Request");
        const auto* version = Header(request, "mcp-protocol-version");
        if (version && !SupportedVersion(*version)) return Error(400, "protocol_version");
        // Without a version header the transport specification infers 2025-03-26.
        // These supported versions share the request/response subset used here.
        const Json params = body.value("params", Json::object());
        if (!params.is_object()) return body.contains("id") ? RpcError(id, -32602, "Invalid params") : Error(400, "invalid_notification");
        if (!body.contains("id")) return {202, "application/json", {}, {}};
        if (method == "initialize") {
            if (!params.contains("protocolVersion") || !params["protocolVersion"].is_string() ||
                !params.contains("capabilities") || !params["capabilities"].is_object() ||
                !params.contains("clientInfo") || !params["clientInfo"].is_object() ||
                !params["clientInfo"].contains("name") || !params["clientInfo"]["name"].is_string() ||
                !params["clientInfo"].contains("version") || !params["clientInfo"]["version"].is_string()) return RpcError(id, -32602, "Invalid params");
            auto selected = params["protocolVersion"].get<std::string>();
            if (!SupportedVersion(selected)) selected = kProtocolVersion;
            return RpcResult(id, {{"protocolVersion", selected}, {"capabilities", {{"tools", Json::object()}}},
                {"serverInfo", {{"name", "FusionMCP"}, {"version", "0.1.0"}}},
                {"instructions", "Standalone FusionMCP. Optional providers may be absent or disabled. Poll queued operations for actual completion."}});
        }
        if (method == "ping") return RpcResult(id, Json::object());
        if (method == "tools/list") {
            if (params.contains("cursor")) return RpcError(id, -32602, "Pagination is not supported");
            return RpcResult(id, {{"tools", Tools(Compatible(resolver_))}});
        }
        if (method == "tools/call") {
            if (!params.contains("name") || !params["name"].is_string() ||
                (params.contains("arguments") && !params["arguments"].is_object())) return RpcError(id, -32602, "Invalid params");
            const auto tool = params["name"].get<std::string>();
            if (tool.empty()||tool.size()>128) return RpcError(id,-32602,"Unknown tool");
            const auto backend=Compatible(resolver_);
            const auto descriptor=Lookup(backend,tool);
            if (descriptor.is_null()) return RpcError(id, -32602, "Unknown tool");
            const auto envelope = Invoke(backend, config_.allowMutations,
                tool, params.value("arguments", Json::object()),descriptor);
            return RpcResult(id, McpResult(envelope));
        }
        return RpcError(id, -32601, "Method not found");
    }
}
