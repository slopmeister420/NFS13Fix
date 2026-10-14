#pragma once

// an online request that fails to send on a reused connection is retried on a fresh one instead of
// failing, which the game would report as a lost autolog connection. safe to call from DllMain.
namespace httpretry {

void Init();

} // namespace httpretry
