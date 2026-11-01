#pragma once
#include "Protocol.h"
#include <memory>

namespace fusionmcp::transport
{
    // Optional loopback listener, started only by explicitly enabled config.
    // RequestStop never joins. Stop is for a
    // dedicated non-owner join worker/destruction outside DllMain only.
    class Server
    {
    public:
        Server();
        ~Server();
        Server(const Server&) = delete;
        Server& operator=(const Server&) = delete;
        bool Start(Config config, ProviderResolver provider) noexcept;
        void RequestStop() noexcept;
        bool Stop() noexcept;
    private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
