#pragma once
#include "core/Json.h"
#include "core/RequestBroker.h"
#include <fusionmcp/ProviderAPI.h>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace fusionmcp {
// Central lifetime, policy and copied-DTO authority. Never pumps an engine owner
// from transport; trusted runtime adapters call Pump on their actual owner.
class ProviderRegistry {
public:
    // Host-internal tool (fusionmcp.* or a host-adapted namespace such as f4fs.*)
    // answered on the caller's thread without the registry lock. Handlers never
    // touch engine objects or wait on an owner.
    struct ControlTool {
        std::string name,description;
        Json schema;
        bool mutating=false;
        std::function<Json(const Json& arguments,bool allowMutations)> handler;
        std::string capability; // Reported as supported by fusionmcp.status when set.
        // Values of the "action" argument that mutate, for tools mixing read and
        // write actions. Gated by the same AllowMutations policy as mutating.
        std::vector<std::string> mutatingActions;
        // Empty when available, else the reason. Called under the registry lock:
        // must not block, wait on an owner or call back into the registry.
        std::function<std::string()> unavailableReason;
    };
    explicit ProviderRegistry(RequestBroker::NowFn now = [] { return RequestClock::now(); });
    ~ProviderRegistry();
    ProviderRegistry(const ProviderRegistry&) = delete;
    ProviderRegistry& operator=(const ProviderRegistry&) = delete;
    FUSIONMCP_Status Register(const FUSIONMCP_ProviderV1* provider, FUSIONMCP_ProviderHandle* handle) noexcept;
    // Host-internal only, never reachable through the C ABI: admits the reserved
    // b21 namespace, starts enabled and cannot be toggled or unregistered by
    // handle. capabilities are reported as supported by fusionmcp.status.
    FUSIONMCP_Status RegisterBuiltin(const FUSIONMCP_ProviderV1* provider, std::vector<std::string> capabilities) noexcept;
    bool RegisterControl(ControlTool tool) noexcept;
    FUSIONMCP_Status SetEnabled(FUSIONMCP_ProviderHandle handle, bool enabled) noexcept;
    FUSIONMCP_Status Unregister(FUSIONMCP_ProviderHandle handle) noexcept;
    void SetOwnerAvailable(Owner owner, bool available);
    void SetMutationsAllowed(bool allowed);
    Json Describe() const;
    Json Lookup(const std::string& name) const; // One copied descriptor or null.
    Json Invoke(const std::string& name, const std::string& arguments, bool allowMutations);
    Json Query(RequestId id);
    Json Cancel(RequestId id);
    // Native tickets must supply their claimed epoch; zero is for a direct
    // trusted owner pump with no previously captured lifecycle authority.
    std::size_t Pump(Owner owner, std::size_t budget = 4, EpochId claimedEpoch = 0);
    EpochId AdvanceEpoch();
    EpochId Epoch() const;
    void Stop();
    std::size_t Pending(Owner owner) const;
private:
    FUSIONMCP_Status Add(const FUSIONMCP_ProviderV1* provider, FUSIONMCP_ProviderHandle* handle, bool builtin, std::vector<std::string> capabilities) noexcept;
    struct State;
    std::shared_ptr<State> state_;
};
}
