# Building v1.7.1

The v1.7.1 release combines the DLSS MFG backend, the built-in menu with its
translations, RTX 50, RTX 30 and experimental RTX 20 support, XeSS Multi Frame
Generation (XeMFG) standalone, through OptiScaler or as OptiScaler's DLSS Frame
Generation provider, including the XeMFG tensor-core kernels, the DLSS 5 stage
and the RTX Hair conversion, in one DLL. ProductVersion is 1.7.1; FileVersion
is 1.7.1.177.

The release target is `RTXMFGUnified`, producing `Release/MFGUnlocker.dll`.
Use Windows x64, Visual Studio 2022/MSVC 14.38.33130, Windows SDK
10.0.22621.0 (including its `dxc.exe`), CMake 4.3.3, and Python 3.10 or newer.

Required external inputs:

- Streamline SDK **2.14.1**.
- Dear ImGui revision `3912b3d9a9c1b3f17431aebafd86d2f40ee6e59c`
  from ReShade 6.8.0, including its backends.
- Vulkan SDK 1.2.176.1 headers and `glslangValidator.exe`.
- Vulkan SDK 1.4.363.0 headers (the XeMFG Vulkan layer; the RTX Hair SPIR-V operand
  table is generated from its `spirv/unified1/spirv.core.grammar.json`, pinned by
  SHA-256) and its `Bin/dxc.exe` with
  `dxcompiler.dll` and `spirv-val.exe`, which compile `dlss5.cpp`'s DLSS 5 passes
  to SPIR-V for the Vulkan stage (`build.ps1 -Dlss5VulkanDxc`; all three pinned
  by SHA-256, and the generator pins every output).
- XeSS SDK **3.0.2** public headers (`inc/xess_fg`, `inc/xell`).
- CUDA Toolkit **13.2** (`nvcc` V13.2.51), for the XeMFG tensor-core kernels
  (`source/native/xess/cuda/xefg_cuda_layers.cu`). `build.ps1` defaults to
  `C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v13.2`; pass `-CudaRoot`
  for another location. No CUDA runtime is linked or shipped: the kernels are
  embedded as one fatbin and run through the NVIDIA driver.
- The pinned XeSS Frame Generation program set: `programs/programs.tsv`
  (SHA-256 `D74BD783D29280ED372653701CE9B0395362E381FAACE1213206E0BDD9BFC9E5`)
  and the 54 DXIL programs it lists. Most are derived from Intel's XeSS SDK;
  see [NOTICE.md](NOTICE.md). They are a separately prepared build input and are
  not included in this source tree.
- The validated SM86 kernel cache, with this directory layout:

```text
native-cache/ampere_native_manifest.inc
native-cache/manifest.json
native-cache/RTX30MFG-Kernels/*.fatbin
native-cache-3109/ampere_native_manifest.inc
native-cache-3109/manifest.json
native-cache-3109/RTX30MFG-Kernels/*.fatbin
all-provider-layouts/native-cache-complete/ampere_native_manifest_additional.inc
all-provider-layouts/native-cache-complete/additional-resources.json
all-provider-layouts/native-cache-complete/RTX30MFG-Kernels/*.fatbin
```

The complete-program cache includes the DLSS Frame Generation 310.9.16 program
(additional program 10, resources 5200-5225).

Kernel caches and the XeSS program set are separately prepared build inputs. A
source checkout alone cannot reproduce the DLL without them. The build script
pins the tools, SDK headers, program set and manifests by SHA-256; CMake also
verifies every embedded kernel's and program's size and hash. No provider DLL is
modified on disk.

With those inputs available, run from the repository root, supplying their
actual paths and a fresh build directory:

```powershell
./source/native/build.ps1 `
    -StreamlineRoot 'C:/SDKs/streamline-sdk-v2.14.1' `
    -ImGuiRoot 'C:/SDKs/reshade-6.8.0/deps/imgui' `
    -NativeCacheRoot 'C:/BuildInputs/rtxmfg-sm86' `
    -XessSdkRoot 'C:/SDKs/xess-sdk-3.0.2' `
    -XessProgramsRoot 'C:/BuildInputs/xess-programs' `
    -BuildDirectory "$PWD/build/v1.7.1" `
    -EnableNgxCreateResultDiagnostics `
    -EnableSessionDiagnostics
```

This reproduces the release configuration: the built-in menu and bounded hook
installation are enabled, with the existing bounded NGX Create result log and
the session diagnostics below.
GPU fault capture, menu suppression, skipped GPU work and test fault injection
remain disabled. Do not enable those diagnostic switches for a release build.

Release, alpha and beta builds add `-EnableSessionDiagnostics`
(`MFG_UNLOCK_SESSION_DIAGNOSTICS`).
The runtime log then stamps every line with local time and opens with the
Windows build, CPU, memory, display adapters and driver versions. It logs
hook-state lines only when they change, and adds a `State` line when the
frame-generation state changes. It records first-chance faults (observed, never
handled) and unhandled exceptions with registers and an unwound stack, and
writes a minidump beside the log (`*.crash.dmp`). A `Session end` record at
process exit names the exiting thread's stack and the last state. A log that
stops without `Session end` was ended abruptly: fail-fast, TerminateProcess or
Task Manager, or a hang. If the runtime log is still held open by another
process, the session writes `<log>.<pid>.log` and says why. The switch is off by
default.

The proxy dispatchers, Vulkan shader arrays, adapted ImGui DX12 and DX11 backends, the
embedded XeSS program table, MFG Unlocker's own XeFG shaders
(`source/native/xess/port/splat_motion_aware.hlsl` and `unet_conv.hlsl`,
compiled with the Windows SDK's `dxc.exe`) and the tensor-core kernel fatbin (sm_75, sm_86, sm_89 and sm_120 SASS,
compiled by `xess/cuda/generate_cuda_layers.py` with the pinned `nvcc` in the
pinned MSVC toolset's environment) are generated in the build directory. The proxy and shader
outputs' normalized SHA-256 values must match
`source/native/generated_sha256.json`; the XeSS generators pin their own inputs
and outputs. The fixed export map is recorded in `proxy_exports.txt`; generation
does not scan DLLs on the build machine. Third-party notices, including the
XeSS program-data notice, remain embedded in the release DLL.

The FSR 3.1 frame interpolation generator (`source/native/fsr/`) needs the AMD FidelityFX SDK v1.1.4 (tag `v1.1.4`, commit
`c6efa6bf7f2027b3ec94f28578bb5965eabb9e55`): point the CMake cache variable `MFG_FIDELITYFX_SDK_ROOT` at the checkout, the folder
that holds `sdk/`. At configure time `fsr/generate_fsr_shaders.py` copies the parts of `sdk/` the build uses into
`generated/fsr/sdk`, applies `fsr/fork.patch` there and compiles the frame interpolation and optical flow permutations with
the SDK's own `sdk/tools/binary_store/FidelityFX_SC.exe` (72 compiles, a few seconds). The SDK checkout itself is never
modified. A stamp of the patch, the script and the SDK's `ffx_frameinterpolation.cpp` lets an unchanged configure skip the
step. `MFG_FSR_SHADER_RUNNER` names the program that runs the Windows compiler on a Linux host and stays empty on Windows.

The Witcher 3 hair converter header is generated from
`source/native/witcher_dots/converter.hlsl` and `converter_source.h.in`.
Generated headers and runtime shader caches are not source inputs to publish.
The hair code discovery in `source/native/witcher_dots/game_discovery.cpp`
decodes instructions with the hde64 decoder bundled with MinHook in
`source/native/third_party/minhook`; it needs no additional build input.

Test projects:

- `tests/release_regression`: menu, loader, control, NVIDIA policy and provider
  gate harnesses. See its [README](tests/release_regression/README.md). They
  must load the freshly built DLL. The Vulkan cases use a real Vulkan device;
  DirectX 12 presentation cases use WARP. They exercise synthetic presentation
  and control paths, not a game's Frame Generation output.
- `tests/provider_audits`: DLSS Frame Generation provider admission audits
  (310.9.0 and 310.9.16, Preset C) against those providers, mapped without
  running provider code. Its RTX 30 audit is built only when configured with the
  same `MFG_AMPERE_*` SM86 cache arguments as the release, and checks that the
  complete 26-slot native program is selected from the embedded kernels.
- `tests/ampere_audits` and `tests/ampere_runtime`: RTX 30 font, publication,
  selection and policy checks with data-only provider images and mock CUDA calls.
  `ampere_runtime`'s CTest runs the batch, options, status, Streamline OTA and
  OptiScaler private Streamline startup checks, which need no inputs, the NVAPI
  factory-detour check, which needs
  the installed NVIDIA driver (a split driver-store `nvapi64_impl.dll`), and the
  GPU name override check, which needs an NVIDIA DXGI adapter and detours the
  real System32 `dxgi.dll` inside its own process only. Its
  flip-metering, wrapper, selection and architecture tests take real
  `sl.dlss_g.dll` wrappers and DLSS-G providers as arguments.
- `tests/blackwell_runtime`: RTX 50 identification and menu policy. Its CTest
  runs the selection, Vulkan admission, Streamline OTA, menu family and Dynamic
  MFG checks, which need no inputs, and the adapter probe, which runs the
  production RTX 50 check against the machine's NVIDIA adapters through the
  installed driver's CUDA (exit 2 without one). RTX 50 runs DLSS-G unmodified,
  so nothing here maps or patches a provider.
- `tests/xess_cuda`: the XeMFG tensor-core route against Intel's DXIL programs,
  through the production route code, on an NVIDIA RTX 20/30/40/50 GPU. Every layer
  program, padded and poisoned outputs, fuzzed and shrunken constants, and the
  fallback for incomplete recording state must be byte-identical. It needs the
  build's fatbin, libxess_fg 3.0.2's layer programs and the pinned program set.
- `tests/intel_native`: the Intel Arc native route's driver contract and XeLL
  pairing rules. It maps the Intel driver's XeFG and XeLL files and the XeSS SDK
  3.0.2 runtime without running their code; configure with `XESS_SDK_ROOT` and
  the `INTEL_*` and `XESS_*` fixture paths.
- `tests/localization`: menu translation lookup and Auto language rules,
  the shipped catalogs and whether the Windows fonts they list draw every
  character, the catalog generator's rejections, and that every catalog
  matches the sources. Configure with `IMGUI_ROOT`.

The menu catalogs in `source/native/locale` are validated and compiled in at
configure time by `source/native/generate_locale.py`; a catalog error fails the
configure. See [locale/README.md](source/native/locale/README.md).

These checks do not establish live provider execution or correct final
presentation in a game.

## Building on Linux

`tools/linux/build.sh` builds `RTXMFGUnified` into `build/linux/MFGUnlocker.dll` with clang-cl and lld-link against
an xwin copy of the Windows CRT and SDK. It does not run MSVC, glslangValidator or the Windows SDK's `dxc.exe`. It does run `nvcc`, for the tensor-core kernels only.
The Windows build above is untouched: `source/native/CMakeLists.txt` only takes the Linux branches when
`MFG_PREGENERATED_INPUTS` is set, and `build.ps1` never sets it.

Tools on the machine: clang-cl, lld-link, llvm-rc, llvm-mt, llvm-ml, llvm-lib, CMake 3.24 or newer, Ninja, Python 3.10
or newer, `spirv-val`, a Linux `dxc` of build 5512-01b62ad4, and an xwin splat of the CRT and SDK in `~/.xwin`. CUDA Toolkit 13.5 (`/opt/cuda/bin/nvcc`, or `MFG_NVCC`) compiles the tensor-core kernels. Wine is
needed only for `test.sh`. The script prints the versions it finds and does not fail on them.

Inputs, each pinned by SHA-256 (the same pins as `build.ps1`, read from it). A mismatch stops the script and names the file:

| Input | Default location | Override |
| --- | --- | --- |
| Streamline SDK 2.14.1 | `~/.cache/mfg-unlocker/sdk/streamline-sdk-v2.14.1` | `MFG_STREAMLINE_ROOT` |
| XeSS SDK 3.0.2 | `~/.cache/mfg-unlocker/sdk/xess-sdk-3.0.2` | `MFG_XESS_SDK_ROOT` |
| FidelityFX SDK v1.1.4 | `~/.cache/mfg-unlocker/sdk/fidelityfx-sdk-v1.1.4` | `MFG_FIDELITYFX_SDK_ROOT` |
| Dear ImGui `3912b3d` | `~/Projetos/renodx/external/reshade/deps/imgui` | `MFG_IMGUI_ROOT` |
| Vulkan 1.2.176 headers | a Vulkan-Headers git checkout with tag `v1.2.176` | `MFG_VULKAN12_HEADERS_GIT` |
| Vulkan 1.4.363 headers and SPIR-V grammar | `/usr/include` (Arch packages `vulkan-headers` and `spirv-headers`) | `MFG_VULKAN14_INCLUDE` |
| Release 1.7.1 zip | `~/.cache/mfg-unlocker/release/v1.7.1/MFGUnlocker-v1.7.1.zip` | `MFG_RELEASE_ZIP` |

The FidelityFX SDK is not pinned by SHA-256 here: the generator's stamp covers the patch, the script and the SDK's
`ffx_frameinterpolation.cpp`, and `git apply` fails if the SDK is not the version the patch was cut from. The SDK's shader
compiler runs under `tools/linux/wine.sh`.

The pins describe the Windows checkouts, which use CRLF. ImGui and the Vulkan headers match only after LF to CRLF, so the
script verifies them in that form and builds from a pinned CRLF copy in `~/.cache/mfg-unlocker/sdk/crlf`. The Vulkan
1.4.363 `vk_video` headers and `spirv/unified1/spirv.core.grammar.json` are not pinned by `build.ps1` (the grammar is
pinned by `source/native/CMakeLists.txt`, in its CRLF form); `tools/linux/check_sdk.py` pins them too. When a pin
list grows, the script stops with "missing"; delete `~/.cache/mfg-unlocker/sdk/crlf` and run it again.

Some inputs need Windows-only tools, which Linux does not have. The script takes them from the release DLL instead:
the 130 SM86 kernels and the RTX 30 manifests, the XeSS programs, the XeFG shaders, the XeMFG
Vulkan SPIR-V, and the overlay and input-copy SPIR-V. `tools/linux/extract_inputs.py` checks the zip's SHA-256 before it
reads anything, refuses any other file, and writes the result once to `~/.cache/mfg-unlocker/inputs/v1.7.1`. Every output is
checked against the pin its own Windows generator carries. The cache holds Intel-derived programs under Intel's terms
(see [NOTICE.md](NOTICE.md)); it stays outside the repository. The proxy dispatchers, the ImGui DX12 adaptation, the
locale catalogs and the DLSS 5 SPIR-V are generated at every configure by the repository's own generators.

A change to `ui_color.frag`, `vk_input_copy.comp` or `xess/port/*.hlsl` makes the script stop: Linux cannot recompile
those, so the extracted outputs would no longer match the source.

The tensor-core kernels (`xess/cuda/xefg_cuda_layers.cu`, `xefg_cuda_params.h`) are the exception: `build.sh` compiles them
with `xess/cuda/generate_cuda_layers.py --linux`, using the same `-gencode` set as the Windows build (sm_75, sm_86, sm_89,
sm_120). The script writes the fatbin, `embedded_xefg_cuda.generated.h` and `embedded_xefg_cuda.rc.in` to
`build/linux/xefg-cuda`, and CMake takes them from there (`MFG_XEFG_CUDA_INPUTS`) instead of from the extracted inputs.
The generator pins the compiler (CUDA 13.5.37) and the fatbin's SHA-256 (`PINNED_FATBIN_LINUX`), next to the Windows pins
(13.2.51 and `PINNED_FATBIN`). A kernel edit changes the fatbin, so the script stops and prints the new digest; update
the pin only after `tests/xess_cuda` passes with that fatbin. The release fatbin stays in the extracted inputs, for
comparison, and CMake refuses to embed it: the kernels' parameter block no longer matches the release's. The Windows pin
was not updated after the kernels changed, so the Windows build stops until someone runs `tests/xess_cuda` there and
re-pins.

`tools/linux/test.sh` builds the test projects that need no game file or provider DLL with the same toolchain and
runs them under Wine in `~/.cache/mfg-unlocker/wineprefix`. It prints one line per test: passed, failed, or not runnable
with the reason. The tests that create a GPU device (a D3D12 or Vulkan device, an XeLL or XeFG context, a CUDA context
or NVAPI calls into the installed driver) do not run by default and show as not runnable; set `MFG_TEST_GPU=1` to run
them, and only with no game or simulator using the GPU. A release_regression case that fails with the built DLL is run
again with the unmodified v1.7.1 DLL; if that fails too, the case is not runnable here. `build/linux-tests` holds the logs.

The tests that need no GPU and the release_regression cases run `MFG_TEST_JOBS` at a time (8 by default), and the table
still lists the tests in their defined order. The GPU tests run one at a time after them, and the `MFG_TEST_PROTON=1`
project after everything else: GPU tests run side by side, or beside the Proton cases, fail to create a Vulkan device or
get too little video memory for XeFG, and pass on their own. `test.sh` and `proton_cuda.sh` run themselves inside
`tools/linux/headless.sh`, a virtual KWin session with its own Xwayland, so the windows and consoles that Wine and Proton
open never reach the desktop or take its focus. `MFG_HEADLESS=0` runs them on the desktop instead.

`MFG_TEST_PROTON=1` adds the `tests/xess_cuda` cases, run under Proton-FS2024 (the Proton that MSFS 2024 uses) instead of
plain Wine, as project `xess_cuda_proton`. It is separate from `MFG_TEST_GPU` and needs the same condition: no game or
simulator using the GPU. `tools/linux/proton_cuda.sh route` does the work. It compiles the kernels from source, builds
`tests/xess_cuda` with the Linux toolchain and runs every case it defines, plain and ReShade-wrapped, in a throwaway Proton
prefix at `~/.cache/mfg-unlocker/proton-cuda-prefix`. The script
refuses to start, and `test.sh` records the project as not runnable, when any process maps a game. ReShade 6.8.0.2155 and
the AtmosphereMod and RenoDX add-ons are copied from the Witcher 3 and MSFS 2024 installs and checked by SHA-256 first.
Intel's layer programs are not in the release DLL, so the first run dumps them: it starts the XeSS SDK's frame generation
sample under Proton for 90 seconds with `VKD3D_SHADER_DUMP_PATH` set, and keeps the programs
the route recognizes in `~/.cache/mfg-unlocker/intel-programs`. The cases write their output to
`build/proton-cuda/route/tests/<case>/output.txt`. The route also runs `XmxTensorCoreContextsTests` from `tests/xmx`,
which `test.sh` builds before it: 16 XeMFG DLSS-G contexts at different sizes, and the sixteenth must still launch on the
tensor cores. Each XeFG context brings 28 layer pipelines, and the route holds up to 512. It also runs the seven `FsrModel_*`
tests of the FSR 3.1 model from `tests/fsr`, one process per test, because they need Proton's D3D12 device.

`tools/linux/proton_cuda.sh profile` measures where the GPU time of one XeFG `execute` goes. It runs `XmxXefgProfile` from
`tests/xmx` under Proton, drives XeFG at MSFS 2024's shapes (3840x2160 output, 2560x1440 render, 3 generated frames, HUD
composite on, 300 pairs after 30 of warmup) and prints a GPU timestamp for every dispatch, per generated frame. Arguments
go straight to the benchmark: `--output WxH`, `--render WxH`, `--frames N`, `--pairs N`, `--warmup N` and `--no-hud`.
`--dxil` keeps the tensor-core route off, so the same run measures the DXIL port, with totals only. `--declined` installs
the route but declines every layer, which gives the DXIL port dispatch by dispatch. `--content zero|scene|noise` chooses
what the input textures hold: zeros (the default), a flight-like frame pair with moving objects, or random texels.
The run also names the program behind every non-layer pass: it hooks pipeline creation, dumps each compute program under
`build/proton-cuda/profile/programs/` and prints `pass <k> -> <file>` for the ones in the known program folders.
`profile --reshade` runs the same benchmark with ReShade's `dxgi.dll` and the route tests' add-ons beside it, so the device
is wrapped as in the game. It needs the same condition as the GPU tests: no game or simulator using the GPU.

Under Proton, vkd3d-proton gives no CUDA object for a buffer view. Under Wine, the route therefore records the GPU
address of each buffer view XeFG writes, and passes weights, scale and bias to the kernels as addresses. Textures and
surfaces stay CUDA objects. On Windows nothing changes.

Differences from the release DLL, checked with `tools/linux/compare_dll.py <release.dll> build/linux/MFGUnlocker.dll`:

- All 1297 exports match by ordinal and name, and the export directory reads `RTXMFG.dll`. lld names the directory after
  the output file, so `finalize_dll.py` rewrites the name after the link, and `/EXPORT:MfgXeFGVkGetApi,@428` and
  `/EXPORT:MfgXeMFGProviderGetApi,@429` give those two exports the ordinals link.exe chose. `/MERGE:.edata=.edata` keeps the export table 4-byte aligned, which
  `SelectProxyExports` in `single_module.cpp` requires.
- The code is built with `/EHa`. clang-cl drops `__try` guards around plain memory reads without it, and the RTX 30
  tests crash on the first guarded read.
- The static CRT is xwin's (UCRT 10.0.26100), so the `kernel32.dll` import list differs. The notices (RCDATA 101 to 104)
  differ only in line endings, and the manifest differs in formatting.
- The DLL is larger than the release (about 10.6 MB against 7.4 MB), and the compiler and linker differ.
- The DLSS 5 Vulkan SPIR-V group (RCDATA 3905) differs whenever `dlss5.cpp` differs from the release's.

Set `MFG_CACHE`, `MFG_INPUTS_DIR`, `MFG_BUILD_DIR`, `MFG_XWIN`, `MFG_DXC`, `MFG_NVCC` or `MFG_WINEPREFIX` to move the other paths.
After a change to `tools/linux/toolchain.cmake`, delete `build/linux` and `build/linux-tests`: CMake keeps the flags from
the first configure.
