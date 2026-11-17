"""Compile the XeFG tensor-core kernels (xess/cuda/xefg_cuda_layers.cu) and embed them as one RCDATA fatbin.

The fatbin holds SASS for sm_75 (RTX 20; m8n8k16 mma), sm_86 (RTX 30; also runs on RTX 40), sm_89
(RTX 40) and sm_120 (RTX 50), no PTX: the driver picks the matching SASS, and an architecture without one
keeps the DXIL port (the route requires compute capability 7.5 with tensor cores, 8.6, 8.9 or 12.0).
On Windows the compiler is the pinned CUDA Toolkit 13.2 nvcc, run in the pinned MSVC toolset's environment, and the
output is pinned by SHA-256 (nvcc output is deterministic), so a different compiler or an edited kernel fails the
build instead of shipping silently. The Linux build (--linux, tools/linux/build.sh) compiles the same source with the
pinned CUDA Toolkit 13.5 nvcc and its own output pin, and writes a pregenerated-style directory: the fatbin under
xess_cuda/, embedded_xefg_cuda.generated.h and embedded_xefg_cuda.rc.in, whose @MFG_XEFG_CUDA_INPUTS@ placeholder is
the directory CMake is given as MFG_XEFG_CUDA_INPUTS. Re-validate with tests/xess_cuda before updating a pin.
"""
from pathlib import Path
import argparse
import collections
import hashlib
import subprocess

PINNED_NVCC = "Cuda compilation tools, release 13.2, V13.2.51"
PINNED_NVCC_LINUX = "Cuda compilation tools, release 13.5, V13.5.37"
RESOURCE_ID = 7300  # XeSS programs use 7000..7054, RTXMFG's XeFG shaders 7100..
SOURCE = "xess/cuda/xefg_cuda_layers.cu"
KERNELS = ["L_aa016078", "L_198d6bf8", "L_dcd8becf", "L_efbe3788", "L_50f0c205", "L_a3c3caa1", "L_9481d452",
           "L_06e97933", "L_3ed03daf", "L_748b0baf", "L_cb391582", "L_fc1b8a04", "L_3459485", "L_3452733",
           "L_flow16_acc17", "L_3453044"]
PINNED_FATBIN = "2ffae52e7878ae7e374ff953b864204f079c3199dc904b2ce7ecf8b37ee130f7"
PINNED_FATBIN_LINUX = "66ffd6ae27714847cd7b641e2bc7aabb5540df5d0f3486a1589ed1d6bda5c61d"
GENCODE = ("-gencode arch=compute_75,code=sm_75 -gencode arch=compute_86,code=sm_86 -gencode arch=compute_89,code=sm_89 "
           "-gencode arch=compute_120,code=sm_120")
LINUX_PLACEHOLDER = "@MFG_XEFG_CUDA_INPUTS@"


def parse_arguments():
    parser = argparse.ArgumentParser()
    parser.add_argument("--nvcc", type=Path, required=True, help="CUDA Toolkit bin/nvcc (13.2 nvcc.exe, or 13.5 nvcc with --linux)")
    parser.add_argument("--linux", action="store_true", help="compile with the Linux nvcc and write the pregenerated-style directory")
    parser.add_argument("--vcvars", type=Path, help="VC/Auxiliary/Build/vcvars64.bat of the build's Visual Studio")
    parser.add_argument("--vcvars-ver", help="MSVC toolset version (VCToolsVersion)")
    parser.add_argument("--source", type=Path, required=True, help="source/native directory")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if args.linux and (args.vcvars or args.vcvars_ver):
        parser.error("--linux takes no --vcvars or --vcvars-ver")
    if not args.linux and not (args.vcvars and args.vcvars_ver):
        parser.error("Windows needs --vcvars and --vcvars-ver")
    return args


def check_nvcc(nvcc, pinned):
    version = subprocess.run([str(nvcc), "--version"], capture_output=True, text=True, check=True).stdout
    if pinned not in version:
        raise SystemExit(f"Unpinned nvcc: {version!r} (expected {pinned!r})")


def compile_windows(args, target):
    command = (f'call "{args.vcvars}" -vcvars_ver={args.vcvars_ver} >nul 2>nul && "{args.nvcc}" -fatbin -O3 '
               f'{GENCODE} -o "{target}" "{args.source / SOURCE}"')
    # One command-line string: cmd.exe /s strips only the outer quotes (a list would escape the inner ones).
    return subprocess.run(f'cmd.exe /d /s /c "{command}"', capture_output=True, text=True)


def compile_linux(args, target):
    return subprocess.run([str(args.nvcc), "-fatbin", "-O3", *GENCODE.split(), "-o", str(target), str(args.source / SOURCE)],
                          capture_output=True, text=True)


def verify_fatbin(target, pinned):
    data = target.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    for kernel in KERNELS:
        if (kernel.encode() + b"\0") not in data:
            raise SystemExit(f"{kernel} missing from {target}")
    if digest != pinned:
        raise SystemExit(f"{SOURCE} compiled to {digest}, pinned {pinned}; update the pin only after "
                         "tests/xess_cuda passes with this fatbin")
    return digest, len(data)


def write_when_changed(path, lines):
    text = "\n".join(lines) + "\n"
    if not path.exists() or path.read_text(encoding="utf-8") != text:
        path.write_text(text, encoding="utf-8")


def output_lines(digest, size, fatbin_reference):
    header = ['#pragma once', '#include "runtime_payload.h"', 'namespace xefg_cuda::embedded {',
              f'inline constexpr xess_payload::Asset fatbin{{{RESOURCE_ID}, "cuda/xefg_cuda_layers.fatbin", "{digest}", {size}}};',
              '}']
    resource = ['#include <windows.h>', 'LANGUAGE LANG_NEUTRAL, SUBLANG_NEUTRAL',
                f'{RESOURCE_ID} RCDATA "{fatbin_reference}"']
    return header, resource


Flavor = collections.namedtuple("Flavor", "nvcc_pin fatbin_pin compile resource_name reference")
WINDOWS = Flavor(PINNED_NVCC, PINNED_FATBIN, compile_windows, "embedded_xefg_cuda.rc",
                 lambda target: target.resolve().as_posix())
LINUX = Flavor(PINNED_NVCC_LINUX, PINNED_FATBIN_LINUX, compile_linux, "embedded_xefg_cuda.rc.in",
               lambda target: f"{LINUX_PLACEHOLDER}/xess_cuda/xefg_cuda_layers.fatbin")


def main():
    args = parse_arguments()
    flavor = LINUX if args.linux else WINDOWS
    check_nvcc(args.nvcc, flavor.nvcc_pin)
    out = args.output / "xess_cuda"
    out.mkdir(parents=True, exist_ok=True)
    target = out / "xefg_cuda_layers.fatbin"
    compiled = flavor.compile(args, target)
    # The compiler's output goes to stdout: the build script treats any stderr text as a failure.
    print((compiled.stdout + compiled.stderr).strip())
    if compiled.returncode != 0:
        raise SystemExit(f"nvcc failed with exit {compiled.returncode}")
    digest, size = verify_fatbin(target, flavor.fatbin_pin)
    header, resource = output_lines(digest, size, flavor.reference(target))
    write_when_changed(args.output / "embedded_xefg_cuda.generated.h", header)
    write_when_changed(args.output / flavor.resource_name, resource)
    print(f"Compiled and embedded the XeFG tensor-core kernels ({len(KERNELS)}) with {flavor.nvcc_pin}")


if __name__ == "__main__":
    main()
