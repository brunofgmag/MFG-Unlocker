# Universal MFG Unlocker V1.7.1

## About this fork

This is a fork of [dashdogy/MFG-Unlocker](https://github.com/dashdogy/MFG-Unlocker) v1.7.1 with three additions:
fixes that let MFG Unlocker run under Proton and Wine on Linux, the XMX generation mode from
[mavismmg/MFGAdaUnlock-RenoDx](https://github.com/mavismmg/MFGAdaUnlock-RenoDx), and an ANR FG generation mode for
Apex Compute's [ANR FG](https://github.com/apex-compute/ANR). The rest of this README is upstream's, and its Quick
start downloads upstream's release, which has none of them. This fork's builds are on its own
[Releases](https://github.com/brunofgmag/MFG-Unlocker/releases) page.

**XMX** runs Intel's XeFG model inside the game's own DLSS Frame Generation on RTX 40 and RTX 50 GPUs. DLSS Frame
Generation still presents the frames and keeps Reflex; XeFG only draws the generated frames. NVIDIA's generator is
never created, which left about 940 MB more VRAM to Microsoft Flight Simulator 2024 at 4K. To turn it on:

1. Download [Intel's XeSS SDK 3.0.2](https://github.com/intel/xess/releases/tag/v3.0.2) and copy `bin/libxess_fg.dll`
   and `bin/libxell.dll` into a folder named `RTXMFG-XeSS` beside MFG Unlocker. XMX needs that `libxess_fg.dll`
   (1.3.1.78); with another version NVIDIA's generator keeps running.
2. In the menu, set **Generation model** to **XMX (Intel XeFG)** and restart the game.

XMX stays off on RTX 30 and RTX 20 GPUs and while XeMFG standalone is on.

**XeLL sleep**, under Generation model, sends the game's Reflex sleep to Intel XeLL while XMX generates. Under Proton it
also keeps the low latency mode off in vkd3d-proton, so vkd3d-proton keeps its own two-frame queue limit and the
launch option `VKD3D_DISABLE_EXTENSIONS=VK_NV_low_latency2` is no longer needed.

**ANR FG** hands the game's DLSS Frame Generation to Apex Compute's ANR FG on RTX 40 and RTX 50 GPUs, in D3D12
games. ANR FG draws the generated frames and DLSS Frame Generation still presents them; MFG Unlocker never creates
NVIDIA's generator. ANR FG is a closed-source beta, and this fork does not ship it. To turn it on:

1. Download an ANR FG package from [ANR's Releases](https://github.com/apex-compute/ANR/releases) and copy the contents
   of its `Apex Neural Rendering FG/dlssg` folder (`nvngx_dlssg.dll`, `nr_fg.ini` and the folders next to them) into
   a folder named `RTXMFG-ANR` beside MFG Unlocker. Don't run ANR FG's own installer in that game; it installs
   OptiScaler. MFG Unlocker loads ANR FG from a copy it writes in that folder, `anr_fg_provider.dll`, because under
   Proton a second DLL named `nvngx_dlssg.dll` can load as NVIDIA's.
2. In the menu, set **Generation model** to **ANR FG** and restart the game.

ANR FG stays off on RTX 30 and RTX 20 GPUs, while XeMFG standalone or XMX is on, and while OptiScaler owns the controls.
If its DLL is missing or fails to start, NVIDIA's generator takes over, and MFG Unlocker's log says why in a line
starting with `ANR`.

**Proton fixes**, each tested in games under Proton:

- The menu and the adapter-parent route work with ReShade loaded as `dxgi.dll`.
- DLSS 5 compiles its compute shaders with Wine's `D3DCompile`
  ([dashdogy/MFG-Unlocker#169](https://github.com/dashdogy/MFG-Unlocker/issues/169)) and opens its provider when Wine
  reports `PAGE_WRITECOPY` ([dashdogy/MFG-Unlocker#170](https://github.com/dashdogy/MFG-Unlocker/issues/170)).
- DLSS 5 accepts depth, motion vectors and exposure with mips, as Microsoft Flight Simulator 2024 sends them
  ([dashdogy/MFG-Unlocker#171](https://github.com/dashdogy/MFG-Unlocker/issues/171)).

The fork also builds on Linux with clang-cl and xwin; see [Building on Linux](BUILD.md#building-on-linux).

---

**README** · [Install](docs/install.md) · [Using the menu](docs/menu.md) · [Game compatibility](docs/compatibility.md) · [OptiScaler](docs/optiscaler.md) · [XeMFG](docs/xemfg.md) · [Troubleshooting](docs/troubleshooting.md)

Multi Frame Generation for Windows x64 games, in one DLL. It brings **DLSS
Multi Frame Generation** controls to RTX 50, RTX 40, RTX 30 and RTX 20 series
GPUs, with fixed multipliers up to 6x and Dynamic MFG where supported, on their
own or through [OptiScaler](docs/optiscaler.md). RTX 50
cards run DLSS MFG natively, so MFG Unlocker changes nothing in NVIDIA's code
there and only adds its menu controls.

It also runs **XeSS Multi Frame Generation (XeMFG)** on NVIDIA, AMD and Intel
GPUs, on its own ([XeMFG standalone](docs/xemfg.md#xemfg-standalone),
experimental) or through OptiScaler, and **DLSS 5** in games that run DLSS
Super Resolution, DLAA or Ray Reconstruction, with an NVIDIA DLSS 5 dll that
supports your GPU ([details](docs/menu.md#dlss-5)).

For DLSS MFG the game must already have a Streamline DLSS Frame Generation
integration. Available modes depend on the GPU, the active runtime and NVIDIA's
per game MFG override maximum. A listed maximum does not guarantee that the
active Frame Generation pipeline can apply it.

If this mod helps you, [help me get through university on Ko-fi](https://ko-fi.com/dashdogy).

## Quick start

1. Download `MFGUnlocker-v1.7.1.zip` from
   [Releases](https://github.com/dashdogy/MFG-Unlocker/releases) and extract it.
2. Rename `MFGUnlocker.dll` to a filename the game loads early, usually
   `version.dll` or `winmm.dll`. [Game compatibility](docs/compatibility.md)
   lists the name for each game.
3. Place it beside the **actual game executable**, not the launcher.
4. Launch the game, enable Frame Generation, and press **Backspace** to open
   the menu.

The [install guide](docs/install.md) has the full steps, every supported
filename, upgrading and uninstalling. To use OptiScaler, see
[OptiScaler](docs/optiscaler.md). MFG Unlocker is not supported in games with
[anti-cheat](docs/compatibility.md#games-with-anti-cheat).

## What's new in V1.7.1

- **DLSS 5 presets:** save your settings under a name and share them as a code
  ([DLSS 5](docs/menu.md#dlss-5)); Auto resolution scale changes smoothly.
- **DirectX 11 games:** the menu and DLSS 5 now run there too.
- **[XeMFG as OptiScaler's DLSS Frame Generation](docs/xemfg.md#xemfg-as-optiscalers-dlss-frame-generation)
  (experimental):** up to 6x and Dynamic MFG in DirectX 12 and Vulkan games,
  with no extra DLL.
- **[RTX Hair on RTX 40 and RTX 30](docs/compatibility.md#path-traced-hair)**,
  on by default, Vulkan games included.
- **An MFG Unlocker tab in OptiScaler's menu**
  ([OptiScaler](docs/optiscaler.md#menus)), and a
  [Keybinds tab](docs/menu.md#keys) in MFG Unlocker's own menu.
- **XeMFG pacing:** a reworked pacer on AMD for smoother frames and lower
  latency.
- **RTX 30 and RTX 20** are no longer held at 2x in Streamline 2.12 games
  (Darktide).

The full list, and every earlier release, is in the [changelog](CHANGELOG.md).

## Reporting problems

Read [Troubleshooting](docs/troubleshooting.md) first. Report bugs in
[Issues](https://github.com/dashdogy/MFG-Unlocker/issues), with the log files
listed there. To report how a game works, use the
[game compatibility report](https://github.com/dashdogy/MFG-Unlocker/issues/new?template=game-compatibility.yml).

## Translation credits

Thanks to Shi (@ shinsir on Discord) for editing the whole Vietnamese
translation, and to Slideulidou (@ slideulidou on Discord) for editing the
French translation.

## Links and licence

The dedicated Cyberpunk 2077 mod is also available on
[Nexus Mods](https://www.nexusmods.com/cyberpunk2077/mods/33286).

Original project code is licensed under the [MIT License](LICENSE).
For source builds, see [BUILD.md](BUILD.md).

Third party components retain their respective licences and notices.
`MFGUnlocker.dll` also embeds XeSS Frame Generation program data derived from
Intel's XeSS SDK. That data is Intel's work, is not covered by the MIT License,
and remains subject to Intel's licence terms. See [NOTICE.md](NOTICE.md).
