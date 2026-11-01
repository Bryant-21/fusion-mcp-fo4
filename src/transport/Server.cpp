// Project-owned GPL-3.0 code; no imported server implementation.
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "Server.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#include <thread>

namespace fusionmcp::transport
{
    namespace
    {
        using Clock = std::chrono::steady_clock;
        struct WsaLease
        {
            bool active = true;
            ~WsaLease() { if (active) WSACleanup(); }
        };
        class Socket
        {
        public:
            explicit Socket(SOCKET socket = INVALID_SOCKET) : value_(socket) {}
            ~Socket() { if (value_ != INVALID_SOCKET) closesocket(value_); }
            Socket(const Socket&) = delete;
            Socket& operator=(const Socket&) = delete;
            SOCKET Get() const noexcept { return value_; }
            SOCKET Release() noexcept { const auto value = value_; value_ = INVALID_SOCKET; return value; }
        private:
            SOCKET value_;
        };
        bool Nonblocking(SOCKET socket)
        {
            u_long mode = 1;
            return ioctlsocket(socket, FIONBIO, &mode) == 0;
        }
        bool Ready(SOCKET socket, bool write, Clock::time_point deadline, const std::atomic_bool& stop, bool* failed = nullptr)
        {
            while (!stop.load(std::memory_order_acquire) && Clock::now() < deadline) {
                fd_set set;
                FD_ZERO(&set);
                FD_SET(socket, &set);
                timeval timeout{0, 100000};
                const int ready = select(0, write ? nullptr : &set, write ? &set : nullptr, nullptr, &timeout);
                if (ready > 0) return true;
                if (ready == SOCKET_ERROR) {
                    if (failed) *failed = true;
                    return false;
                }
            }
            return false;
        }
        bool Send(SOCKET socket, const Response& response, const std::atomic_bool& stop)
        {
            const auto bytes = EncodeResponse(response);
            std::size_t sent = 0;
            const auto deadline = Clock::now() + std::chrono::seconds(5);
            while (sent < bytes.size() && Ready(socket, true, deadline, stop)) {
                const auto chunk = static_cast<int>(std::min<std::size_t>(bytes.size() - sent, 16384));
                const int count = send(socket, bytes.data() + sent, chunk, 0);
                if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
                if (count <= 0) return false;
                sent += static_cast<std::size_t>(count);
            }
            return sent == bytes.size();
        }
        void Client(SOCKET socket, const Dispatcher& dispatcher, const std::atomic_bool& stop) noexcept
        {
            try {
                std::string bytes;
                bytes.reserve(kMaxHeaderBytes + kMaxBodyBytes);
                std::size_t headerEnd = std::string::npos;
                std::size_t bodyBytes = 0;
                Request request;
                // One overall read deadline: a slow stream cannot extend it by
                // repeatedly sending a single byte before a per-recv timeout.
                const auto deadline = Clock::now() + std::chrono::seconds(3);
                while (Ready(socket, false, deadline, stop)) {
                    char buffer[4096];
                    const int count = recv(socket, buffer, sizeof(buffer), 0);
                    if (count == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) continue;
                    if (count <= 0) return;
                    bytes.append(buffer, static_cast<std::size_t>(count));
                    if (headerEnd == std::string::npos) {
                        const auto separator = bytes.find("\r\n\r\n");
                        if (separator != bytes.npos) {
                            headerEnd = separator + 4;
                            if (headerEnd > kMaxHeaderBytes || !ParseHead(bytes.substr(0, headerEnd), request, bodyBytes)) {
                                Send(socket, {400, "application/json", "{\"error\":\"invalid_http\"}", {}}, stop);
                                return;
                            }
                        } else if (bytes.size() > kMaxHeaderBytes) {
                            Send(socket, {413, "application/json", "{\"error\":\"header_limit\"}", {}}, stop);
                            return;
                        }
                    }
                    if (headerEnd != std::string::npos) {
                        if (bytes.size() > headerEnd + bodyBytes) {
                            Send(socket, {400, "application/json", "{\"error\":\"extra_http_bytes\"}", {}}, stop);
                            return;
                        }
                        if (bytes.size() == headerEnd + bodyBytes) {
                            request.body = bytes.substr(headerEnd);
                            Send(socket, dispatcher.Handle(request), stop);
                            return;
                        }
                    }
                }
            } catch (...) { /* Close this connection. No request logging. */ }
        }
    }
    struct Server::State
    {
        std::mutex lifecycle;
        std::atomic_bool stop{false};
        std::thread worker;
    };
    Server::Server() : state_(std::make_unique<State>()) {}
    Server::~Server() { Stop(); }
    bool Server::Start(Config config, ProviderResolver provider) noexcept
    {
        try {
            std::scoped_lock lock(state_->lifecycle);
            if (state_->stop.load(std::memory_order_acquire) || state_->worker.joinable() || !config.enabled || config.port == 0) return false;
            const auto port = config.port;
            const auto dispatcher = std::make_shared<Dispatcher>(std::move(config), std::move(provider));
            WSADATA data{};
            if (WSAStartup(MAKEWORD(2, 2), &data) != 0) return false;
            WsaLease lease;
            Socket listener(::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP));
            const BOOL exclusive = TRUE;
            sockaddr_in address{};
            address.sin_family = AF_INET;
            address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
            address.sin_port = htons(port);
            if (listener.Get() == INVALID_SOCKET ||
                setsockopt(listener.Get(), SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<const char*>(&exclusive), sizeof(exclusive)) != 0 ||
                bind(listener.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
                listen(listener.Get(), 4) != 0 || !Nonblocking(listener.Get())) {
                return false;
            }
            // One-shot lifecycle: a concurrent RequestStop is never reset.
            const auto handle = listener.Get();
            try {
                state_->worker = std::thread([state = state_.get(), dispatcher, handle]() noexcept {
                    {
                        WsaLease workerLease;
                        Socket socket(handle);
                        try {
                            while (!state->stop.load(std::memory_order_acquire)) {
                                dispatcher->Tick();
                                bool failed = false;
                                if (!Ready(handle, false, Clock::now() + std::chrono::milliseconds(100), state->stop, &failed)) {
                                    if (failed) {
                                        state->stop.store(true, std::memory_order_release);
                                        break;
                                    }
                                    continue;
                                }
                                sockaddr_in peer{};
                                int peerSize = sizeof(peer);
                                Socket client(accept(handle, reinterpret_cast<sockaddr*>(&peer), &peerSize));
                                if (client.Get() == INVALID_SOCKET || peer.sin_family != AF_INET ||
                                    peer.sin_addr.s_addr != htonl(INADDR_LOOPBACK) || !Nonblocking(client.Get())) continue;
                                Client(client.Get(), *dispatcher, state->stop);
                            }
                        } catch (...) { /* Worker owns every socket until closed. */ }
                    }
                });
                listener.Release();
                lease.active = false;
            } catch (...) {
                return false;
            }
            return true;
        } catch (...) { return false; }
    }
    void Server::RequestStop() noexcept {state_->stop.store(true,std::memory_order_release);}
    bool Server::Stop() noexcept
    {
        try {
            std::scoped_lock lock(state_->lifecycle);
            if (state_->worker.joinable() && state_->worker.get_id() == std::this_thread::get_id()) return false;
            RequestStop();
            if (state_->worker.joinable()) state_->worker.join();
            return true;
        } catch (...) { return false; }
    }
}
