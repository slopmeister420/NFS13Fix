#pragma once
#include <d3d11.h>
#include <cstdint>

// temporal upscaling (DLSS or FSR) through the 64-bit NFS13Upscaler helper. the caller renders
// at RenderSize() with Jitter() added to its projection, then hands colour and depth to Run().
namespace upscale {

enum class Backend : uint32_t { None = 0, DLSS = 1, FSR = 2 };

struct Config {
    Backend backend = Backend::DLSS;
    uint32_t quality = 1;      // 0 dlaa/native, 1 quality, 2 balanced, 3 performance, 4 ultra performance
    float sharpness = 0.0f;    // fsr only
    bool hdr = false;          // colour is linear hdr rather than display-referred
    bool depthInverted = false;
    const wchar_t* helperDir = nullptr; // folder holding NFS13Upscaler.exe and the sdk dlls
    float jitterSignX = 1.0f, jitterSignY = 1.0f; // jitter sign handed to the sdk
    bool log = false;          // the helper writes NFS13Upscaler.log
};

struct Frame {
    ID3D11ShaderResourceView* color; // render resolution, display-referred unless Config::hdr
    ID3D11ShaderResourceView* depth; // render resolution, device depth
    ID3D11ShaderResourceView* stencil; // render resolution, the g-buffer's stencil (object class), or null
    ID3D11ShaderResourceView* particles; // render resolution, premultiplied particles (alpha = coverage), or null
    float reproject[16];             // row-major, unjittered clip space current -> previous
    float objects[128][16];          // per stencil value - 128 (moving objects), as reproject
    float jitterX, jitterY;          // the Jitter() used for this frame, in pixels
    bool reset;                      // camera cut: drop history
    float frameTimeMs;
    float cameraNear, cameraFar, fovY;
};

// launches the helper; false if it cannot start (the game then renders as usual)
bool Init(ID3D11Device* device, const Config& config);
void Shutdown();

// switches backend and quality; resources are recreated on the next Run
void Reconfigure(Backend backend, uint32_t quality);

// render size for the display size and quality
void RenderSize(uint32_t displayWidth, uint32_t displayHeight, uint32_t& width, uint32_t& height);
void RenderSize(uint32_t quality, uint32_t displayWidth, uint32_t displayHeight, uint32_t& width, uint32_t& height);

// sub-pixel jitter for the given frame, in render pixels (halton 2,3)
void Jitter(uint64_t frame, uint32_t renderWidth, uint32_t displayWidth, float& x, float& y);

// true once the helper runs the configured backend at this display size
bool Ready(uint32_t displayWidth, uint32_t displayHeight, DXGI_FORMAT colorFormat);

// upscales into a display-sized texture of outputFormat (colorFormat when unknown) and returns its
// view, or nullptr if the helper is not ready (the caller falls back to its own path). gpu-side
// waits only.
ID3D11ShaderResourceView* Run(ID3D11DeviceContext* ctx, const Frame& frame, uint32_t displayWidth,
                              uint32_t displayHeight, DXGI_FORMAT colorFormat,
                              DXGI_FORMAT outputFormat = DXGI_FORMAT_UNKNOWN);

// the output texture behind Run's view, for CopyResource
ID3D11Texture2D* Output();

// helper status for logs
const char* Status();

} // namespace upscale
