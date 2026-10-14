#include <windows.h>

#include "borderless.h"
#include "config.h"
#include "framerate.h"
#include "httpretry.h"
#include "input.h"
#include "log.h"
#include "modloader.h"
#include "speedlist.h"
#include "standin.h"
#include "upscaling.h"
#include "volume.h"

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        const config::Settings& cfg = config::Load(module);
        fixlog::Init(module, cfg.log);
        LOGF("=== NFS13Fix ===");
        LOGF("[ini] FrameRate=%d Borderless=%d SDLInput=%d ModLoader=%d Upscaler=%d UpscaleQuality=%d Sharpness=%d",
             cfg.frameRate, cfg.borderless, cfg.sdlInput, cfg.modLoader, cfg.upscaler, cfg.upscaleQuality, cfg.sharpness);
        if (cfg.modLoader)
            modloader::Init();
        if (cfg.borderless)
            borderless::Init();
        if (cfg.sdlInput)
            input::Init();
        framerate::Init(cfg.frameRate);
        volume::Init();
        httpretry::Init();
        if (cfg.soloMP)
            speedlist::Init();
        if (cfg.standInCar > 0)
            standin::Init(static_cast<uint32_t>(cfg.standInCar));
        upscaling::Init(module, cfg.upscaler, cfg.upscaleQuality, cfg.sharpness / 100.0f, cfg.log);
    }
    return TRUE;
}
