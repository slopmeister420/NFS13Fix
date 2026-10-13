# NFS13Fix

NFS13Fix is a plugin for the PC version of Need for Speed: Most Wanted (2012) that fixes several long-standing issues with the port
and adds a few modern options. The game runs in a borderless window instead of exclusive fullscreen, the 60 FPS cap is removed
without the game speeding up, and any controller supported by SDL works out of the box.
It also adds DLSS and FSR, and a simple mod loader: files placed in a `mods` folder next to the game replace the game's own files,
with no repacking needed. SpeedLists can also be played alone in friends-only and invite-only online games.

## Features

* Borderless window
* Unlocked frame rate
* SDL controller input
* DLSS and FSR
* Mod loader
* Solo SpeedLists in private games

## Installation

Install [Ultimate ASI Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader/releases) (32-bit)

Then extract the release archive into the game directory. Settings are stored in `NFS13Fix.ini`, created on first launch.

## Controllers

To use the SDL controller support, turn off Steam Input for the game (Steam > the game's Properties > Controller >
Disable Steam Input). With Steam Input on, Steam takes over the controller and the game only sees Steam's own virtual pad.

## The mods folder

The release ships with a `mods` folder that adds upscaling options to Settings > Graphics. If another mod already replaces the settings
screen or the game's text, leave it out; upscaling can still be configured in `NFS13Fix.ini`.

## Compilation requirements

* Visual Studio 2022 with the C++ desktop workload, CMake 3.21 or newer and the Windows SDK.
* SDL3, included as a submodule. Clone with `--recurse-submodules`.
* libmem, the DLSS SDK and the FidelityFX SDK, downloaded into `external` by `fetch_deps.ps1`.

Run `fetch_deps.ps1`, then `build.ps1`. Use `build.ps1 -Deploy -GameDir <path>` to copy the result into the game directory.

## License

NFS13Fix is licensed under the AGPL-3.0, as it links [libmem](https://github.com/rdbo/libmem).
