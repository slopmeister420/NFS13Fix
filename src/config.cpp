#include "config.h"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

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
    "; files under the mods folder override the game's own files\r\n"
    "ModLoader=1\r\n"
    "\r\n"
    "; SpeedLists can be played alone in friends-only and invite-only games\r\n"
    "SoloMP=1\r\n"
    "\r\n"
    "; upscaler settings\r\n"
    "Upscaler=1\r\n"
    "UpscaleQuality=0\r\n"
    "\r\n"
    "; write NFS13Fix.log\r\n"
    "Log=0\r\n";

// writes the ini in the current layout when it is missing or lacks a current key, keeping the
// user's values and any keys the layout does not know
void Upgrade(const char* ini)
{
    std::string out, keys;
    bool missing = false;
    char value[256];
    for (const char* line = kDefaults; *line;) {
        const char* end = strstr(line, "\r\n");
        std::string l(line, end);
        line = end + 2;
        const size_t eq = l.find('=');
        if (l[0] != ';' && l[0] != '[' && eq != std::string::npos) {
            const std::string key = l.substr(0, eq);
            keys += "|" + key + "|";
            if (GetPrivateProfileStringA("Fix", key.c_str(), "\x01", value, sizeof(value), ini) && value[0] != 1)
                l = key + "=" + value;
            else
                missing = true;
        }
        out += l + "\r\n";
    }
    if (!missing)
        return;

    std::vector<char> section(32768);
    GetPrivateProfileSectionA("Fix", section.data(), static_cast<DWORD>(section.size()), ini);
    for (const char* kv = section.data(); *kv; kv += strlen(kv) + 1) {
        const char* eq = strchr(kv, '=');
        if (eq && keys.find("|" + std::string(kv, eq) + "|") == std::string::npos)
            out += std::string(kv) + "\r\n";
    }

    FILE* f = nullptr;
    if (fopen_s(&f, ini, "wb") == 0 && f) {
        fwrite(out.data(), 1, out.size(), f);
        fclose(f);
    }
}

} // namespace

const Settings& Load(HMODULE self)
{
    char* ini = g_ini;
    GetModuleFileNameA(self, ini, MAX_PATH);
    if (char* dot = strrchr(ini, '.'))
        strcpy_s(dot, ini + MAX_PATH - dot, ".ini");

    Upgrade(ini);

    auto get = [&](const char* key, int def) {
        return static_cast<int>(GetPrivateProfileIntA("Fix", key, def, ini));
    };
    g_settings.frameRate = get("FrameRate", 0);
    g_settings.borderless = get("Borderless", 1) != 0;
    g_settings.sdlInput = get("SDLInput", 1) != 0;
    g_settings.modLoader = get("ModLoader", 1) != 0;
    g_settings.soloMP = get("SoloMP", 1) != 0;
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
