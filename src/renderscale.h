#pragma once
#include <cstdint>

// renders the 3d scene at a size of the caller's choosing; the hud stays at display size
namespace renderscale {

using SizeFn = void (*)(uint32_t displayW, uint32_t displayH, uint32_t& renderW, uint32_t& renderH);

void Init(SizeFn size);

// told the display size each time the game rebuilds its scene targets (a resolution change)
using WatchFn = void (*)(uint32_t displayW, uint32_t displayH);
void Watch(WatchFn watch);
uint32_t DisplayWidth();

} // namespace renderscale
