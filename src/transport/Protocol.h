#pragma once

// Project-owned GPL-3.0 code. This layer never starts a network service.
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace fusionmcp::transport
{
    inline constexpr std::size_t kMaxHeaderBytes = 16384;
    inline constexpr std::size_t kMaxBodyBytes = 65536;
    inline constexpr std::size_t kMaxResponseBytes = 2 * 1024 * 1024;
    inline constexpr const char* kProtocolVersion = "2025-11-25";

    struct Config
    {
        bool enabled = false;
        bool allowMutations = false;
        std::uint16_t port = 8922;
    };
    struct Request
    {
        std::string method;
        std::string target;
        std::vector<std::pair<std::string, std::string>> headers;
        std::string body;
    };
    struct Response
    {
        unsigned status = 500;
        std::string contentType = "application/json";
        std::string body;
        std::vector<std::pair<std::string, std::string>> headers;
    };

    // Internal copied-DTO facade; independent of every optional provider DLL.
    struct Backend
    {
        std::function<std::string()> describe;
        std::function<std::string(const std::string&, const std::string&, bool)> invoke;
        // Only trusted copied-policy work. Posts owner wakes, never runs engine
        // callbacks on transport. Used for admitted async progress/deadlines.
        std::function<void()> tick;
        std::function<std::string(const std::string&)> lookup;
    };
    using ProviderResolver = std::function<std::shared_ptr<const Backend>()>;
    // Strict HTTP/1.1 framing. Duplicate headers and ambiguous length/encoding
    // are refused. The socket layer reads at most these declared bounds.
    bool ParseHead(const std::string& head, Request& request,
        std::size_t& bodyBytes) noexcept;
    std::string EncodeResponse(const Response& response);

    class Dispatcher
    {
    public:
        Dispatcher(Config config, ProviderResolver resolver);
        Response Handle(const Request& request) const noexcept;
        void Tick() const noexcept;
    private:
        Config config_;
        ProviderResolver resolver_;
        Response HandleChecked(const Request& request) const;
    };
}
