#pragma once

// redirects file opens into <gamedir>\mods, so mods\VEHICLES\VEH_383026_HI.BNDL overrides
// VEHICLES\VEH_383026_HI.BNDL without touching the game's own files. spawns its own thread;
// safe to call from DllMain.
namespace modloader {

void Init();

} // namespace modloader
