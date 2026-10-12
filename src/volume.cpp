// The game's waveOut audio output sets its device volume to a hardcoded 90%, and on Windows a
// waveOut device volume is the app's mixer volume. The call is swallowed so the mixer keeps
// whatever the user set.

#include "volume.h"
#include "iat.h"
#include "log.h"

#include <windows.h>
#include <mmsystem.h>

namespace volume {
namespace {

iat::Hook g_setVolume;

MMRESULT WINAPI HookedWaveOutSetVolume(HWAVEOUT, DWORD)
{
    return MMSYSERR_NOERROR;
}

DWORD WINAPI Startup(LPVOID)
{
    const bool ok = iat::HookImport("WINMM.dll", "waveOutSetVolume", 0,
                                    reinterpret_cast<uintptr_t>(&HookedWaveOutSetVolume), 60000,
                                    g_setVolume);
    LOGF("[iat] WINMM.dll!waveOutSetVolume %s", ok ? "hooked" : "NOT FOUND");
    return 0;
}

} // namespace

void Init()
{
    if (HANDLE t = CreateThread(nullptr, 0, Startup, nullptr, 0, nullptr))
        CloseHandle(t);
}

} // namespace volume
