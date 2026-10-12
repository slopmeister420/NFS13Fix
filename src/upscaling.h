#pragma once
#include <windows.h>

// dlss (backend 1) or fsr (backend 2) upscaling of the 3d scene, run by the 64-bit
// NFS13Upscaler helper in the NFS13Upscaler folder next to the asi. quality: 0 native (dlaa /
// fsr native aa), 1 quality, 2 balanced, 3 performance, 4 ultra performance.
namespace upscaling {

void Init(HMODULE self, int backend, int quality, float sharpness, bool log);

} // namespace upscaling
