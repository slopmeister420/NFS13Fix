#include "config.h"

#include <cstdio>
#include <cstring>

namespace config {
namespace {

Settings g_settings;
char g_ini[MAX_PATH] = "";

const char kDefaults[] =
    "[Fix]\r\n"
    "; simulation and frame rate cap in Hz: 0 = monitor refresh rate\r\n"
    "FrameRate=0\r\n"
    "\r\n"
    "; borderless\r\n"
    "Borderless=1\r\n"
    "\r\n"
    "; any controller SDL supports plays as an xbox pad\r\n"
    "SDLInput=1\r\n"
    "\r\n"
    "; 1 = files under the mods folder override the game's own files\r\n"
    "ModLoader=1\r\n"
    "\r\n"
    "; upscaler settings\r\n"
    "Upscaler=1\r\n"
    "UpscaleQuality=0\r\n"
    "\r\n"
    "; 1 = write NFS13Fix.log\r\n"
    "Log=0\r\n";

} // namespace

const Settings& Load(HMODULE self)
{
    char* ini = g_ini;
    GetModuleFileNameA(self, ini, MAX_PATH);
    if (char* dot = strrchr(ini, '.'))
        strcpy_s(dot, ini + MAX_PATH - dot, ".ini");

    if (GetFileAttributesA(ini) == INVALID_FILE_ATTRIBUTES) {
        FILE* f = nullptr;
        if (fopen_s(&f, ini, "wb") == 0 && f) {
            fwrite(kDefaults, 1, sizeof(kDefaults) - 1, f);
            fclose(f);
        }
    }
    // older ini files: add the log switch so it can be found
    char probe[8];
    if (!GetPrivateProfileStringA("Fix", "Log", "", probe, sizeof(probe), ini)) {
        FILE* f = nullptr;
        if (fopen_s(&f, ini, "ab") == 0 && f) {
            const char add[] = "\r\n; 1 = write NFS13Fix.log\r\nLog=0\r\n";
            fwrite(add, 1, sizeof(add) - 1, f);
            fclose(f);
        }
    }

    auto get = [&](const char* key, int def) {
        return static_cast<int>(GetPrivateProfileIntA("Fix", key, def, ini));
    };
    g_settings.frameRate = get("FrameRate", 0);
    g_settings.borderless = get("Borderless", 1) != 0;
    g_settings.sdlInput = get("SDLInput", 1) != 0;
    g_settings.modLoader = get("ModLoader", 1) != 0;
    g_settings.upscaler = get("Upscaler", 1);
    g_settings.upscaleQuality = get("UpscaleQuality", 0);
    g_settings.sharpness = get("Sharpness", 0);
    g_settings.log = get("Log", 0) != 0;
    return g_settings;
}

void Save(const char* key, int value)
{
    if (!g_ini[0])
        return;
    char v[16];
    snprintf(v, sizeof(v), "%d", value);
    WritePrivateProfileStringA("Fix", key, v, g_ini);
}

} // namespace config
