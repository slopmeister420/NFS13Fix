#pragma once
#include <windows.h>

// NFS13Fix.ini next to the asi. written with the defaults when it does not exist.
namespace config {

struct Settings {
    int frameRate = 0;      // 0 = monitor refresh rate, 60 = stock
    bool borderless = true;
    bool sdlInput = true;
    bool modLoader = true;
    bool soloMP = true; // speedlists alone in friends-only / invite-only games
    int standInCar = 1399539; // replaces a car without a bundle (ford focus st), 0 = off
    int upscaler = 1;       // 0 off, 1 dlss, 2 fsr
    int upscaleQuality = 0; // 0 native aa, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    int sharpness = 0;      // fsr sharpening, percent
    bool log = false;       // NFS13Fix.log and NFS13Upscaler.log
};

const Settings& Load(HMODULE self);

// writes one [Fix] key back to the ini
void Save(const char* key, int value);

} // namespace config
