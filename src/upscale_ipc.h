#pragma once
#include <cstdint>

// shared memory between the 32-bit plugin and the 64-bit upscaler helper. fixed-width fields
// only, so both sides agree on the layout.
namespace upscale_ipc {

constexpr uint32_t kMagic = 0x55333146; // "F13U"
constexpr uint32_t kVersion = 2;
constexpr uint32_t kSlots = 8;

enum Backend : uint32_t { kNone = 0, kDLSS = 1, kFSR = 2 };

enum State : uint32_t {
    kStarting = 0, // helper not attached yet
    kReady = 1,    // backend created for the current resources
    kFailed = 2,   // see error
};

// resources the plugin creates and shares; recreated (generation++) on resize or backend change
struct Resources {
    uint32_t generation;
    uint32_t backend;
    uint32_t quality;       // 0 dlaa/native, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    uint32_t hdr;           // colour is linear hdr rather than display-referred ldr
    uint32_t renderWidth;   // maximum render size
    uint32_t renderHeight;
    uint32_t displayWidth;
    uint32_t displayHeight;
    uint32_t colorFormat;   // DXGI_FORMAT of colour and output
    uint32_t depthInverted;
    // NT handles already duplicated into the helper process
    uint64_t color, depth, motion, output, fence;
    uint64_t reactive;      // r8: particle coverage, for fsr
};

struct Frame {
    uint64_t fenceValue;    // inputs ready at this value; helper signals fenceValue + 1
    uint32_t generation;
    uint32_t renderWidth, renderHeight;
    uint32_t reset;
    float jitterX, jitterY; // pixels, render resolution
    float mvScaleX, mvScaleY;
    float sharpness;
    float frameTimeMs;
    float cameraNear, cameraFar, fovY;
    float preExposure;
};

struct Shared {
    uint32_t magic;
    uint32_t version;
    uint64_t adapterLuid;   // helper creates its device on this adapter
    uint32_t parentPid;
    uint32_t helperPid;
    volatile uint32_t state;
    volatile uint32_t readyGeneration; // generation the helper last created a backend for
    volatile uint32_t available;       // bit per Backend the helper can run on this machine
    volatile uint64_t submitted;       // last fenceValue the plugin queued
    volatile uint64_t completed;       // last fenceValue the helper finished
    Resources resources;
    Frame frames[kSlots];              // indexed by (fenceValue / 2) % kSlots
    char error[256];
    char log[1024];
};

} // namespace upscale_ipc
