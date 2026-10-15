#pragma once
#include <windows.h>

namespace borderless {

// turns MW2012's exclusive fullscreen into a borderless window: the whole monitor, or the
// game's resolution centred on it.
// spawns its own thread; safe to call from DllMain.
void Init();

// the game's resolution changed (render targets rebuilt at this display size)
void SetMode(UINT w, UINT h);

} // namespace borderless
