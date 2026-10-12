#pragma once

// stops the game setting its Windows mixer volume to 90% on every launch. spawns its own
// thread; safe to call from DllMain.
namespace volume {

void Init();

} // namespace volume
