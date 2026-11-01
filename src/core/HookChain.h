#pragma once
// Project-owned GPL-3.0 code. Decides whether a vtable slot may be detoured by
// calling through whatever it currently holds.
#include <cstdint>

namespace fusionmcp {
// The engine function, or another loaded plugin's hook (its module is not the
// game's), can be chained. Any other game-image address, or memory owned by no
// module, means a different runtime or layout.
[[nodiscard]] inline bool ChainableSlot(std::uintptr_t current,std::uintptr_t engine,const void* currentModule,const void* gameModule) noexcept {
    return current==engine||(currentModule!=nullptr&&currentModule!=gameModule);
}
}
