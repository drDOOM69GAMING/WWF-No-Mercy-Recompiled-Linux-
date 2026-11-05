# WWF No Mercy Recompiled for Linux

This is an unofficial native Linux version of [WWF No Mercy: Recompiled](https://github.com/jessetbh/WWFNoMercyRecomp). The upstream project did not have a Linux release. This repository adds one for CachyOS and Arch Linux.

The game is statically recompiled with N64Recomp and uses RT64 for graphics. It is not an emulator. You need your own WWF No Mercy USA Rev 1 ROM. The ROM and game textures are not included.

## Download and run

Download `NoMercyRecompiled` from [Releases](https://github.com/drDOOM69GAMING/WWF-No-Mercy-Recompiled-Linux-/releases), then run:

```sh
chmod +x NoMercyRecompiled
./NoMercyRecompiled
```

Choose your USA Rev 1 ROM on first launch. The game remembers it. The executable carries the UI files and controller database it needs, then unpacks them. The ROM, saves, and settings stay in:

```text
~/.config/WWF_No_Mercy/
```

Textures are read from your ROM at runtime. They are not dumped or bundled with the app.

## Xbox controls

- Left stick moves
- D-pad uses the four C-button actions
- Right stick taunts and performs special moves
- A grapples
- X punches and kicks
- LT and RT are L and R
- Right-stick click is Z
- Menu pauses

F11 and Alt+Enter toggle fullscreen. A left-button double-click also toggles fullscreen.

## Requirements

- 64-bit Linux. Tested on CachyOS and intended for Arch Linux.
- A Vulkan-capable graphics driver.
- SDL2, FreeType, and GTK3 runtime libraries.
- A working audio service. PipeWire is supported through SDL.

This Linux build does not include the upstream Windows CRT filter, ray tracing, or HDR options.

## Build from source

Building is not needed to play. To build, see [BUILDING.md](BUILDING.md). Building the game code requires your own USA Rev 1 ROM. The ROM is used to generate the recompiled code and must not be shared.

The Linux changes, dependency patches, and single-file packer are included here. The packer puts the UI files and controller database into the executable. It never adds the ROM or textures.

## About this port

This repository is an independent Linux port based on the GPL-3.0 licensed upstream project. It is not an official release from the upstream developers. Please report Linux-specific problems in this repository's Issues.
