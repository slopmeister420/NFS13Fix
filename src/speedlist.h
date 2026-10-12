#pragma once

// lets a SpeedList start and keep going with one player, but only in friends-only or invite-only
// online games. public games keep the stock rule. safe to call from DllMain.
namespace speedlist {

void Init();

} // namespace speedlist
