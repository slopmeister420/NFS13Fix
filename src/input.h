#pragma once

// SDL3 drives the game's XInput imports, so any controller SDL knows (DualSense,
// DualShock, Switch Pro, 8BitDo, ...) plays as an Xbox pad, rumble included. keyboard
// and controller input only reach the game while its window has focus.
// spawns its own threads; safe to call from DllMain.
namespace input {

void Init();

} // namespace input
