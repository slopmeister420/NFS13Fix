#pragma once
#include <cstdint>

// a car the game has no bundle for (_HI for the player, _LO for other racers), e.g. another player's
// add-on car, is replaced by a stand-in instead of loading forever. safe to call from DllMain.
namespace standin {

void Init(uint32_t standInId);

} // namespace standin
