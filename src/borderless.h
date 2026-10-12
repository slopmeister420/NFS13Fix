#pragma once
#include <windows.h>

namespace borderless {

// turns MW2012's exclusive fullscreen into a borderless window covering the monitor.
// spawns its own thread; safe to call from DllMain.
void Init();

} // namespace borderless
