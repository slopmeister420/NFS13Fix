#pragma once

// unlocks the frame rate without speeding the game up: the simulation tick rate is raised from
// 60 to rate (0 = the monitor's refresh rate, 60 = stock) and each tick covers that much less
// time. safe to call from DllMain.
namespace framerate {

void Init(int rate);

} // namespace framerate
