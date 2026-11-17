import argparse
import hashlib
import os
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

STAGED_FOLDERS = [
    "include",
    "src/components/frameinterpolation",
    "src/components/opticalflow",
    "src/backends/dx12",
    "src/backends/shared",
    "src/shared",
    "libs",
]

FORK_MARKER = ("include/FidelityFX/host/ffx_frameinterpolation.h", "generatedFrameCount")

VARIANTS = [
    ("", ["-DFFX_HALF=0", "-DFFX_HLSL_SM=62", "-T", "cs_6_2"]),
    ("_wave64", ["-DFFX_HALF=0", "-DFFX_PREFER_WAVE64=[WaveSize(64)]", "-DFFX_HLSL_SM=66", "-T", "cs_6_6"]),
    ("_16bit", ["-DFFX_HALF=1", "-enable-16bit-types", "-DFFX_HLSL_SM=62", "-T", "cs_6_2"]),
    ("_wave64_16bit", ["-DFFX_HALF=1", "-enable-16bit-types", "-DFFX_PREFER_WAVE64=[WaveSize(64)]",
                       "-DFFX_HLSL_SM=66", "-T", "cs_6_6"]),
]

COMMON_ARGUMENTS = ["-reflection", "-deps=gcc", "-DFFX_GPU=1", "-E", "CS", "-Wno-for-redefinition",
                    "-Wno-ambig-lit-shift", "-DFFX_HLSL=1"]

FRAME_INTERPOLATION_ARGUMENTS = [
    "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_ACCUMULATE_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_REPROJECT_SAMPLERS_USE_DATA_HALF=1",
    "-DFFX_FRAMEINTERPOLATION_OPTION_POSTPROCESSLOCKSTATUS_SAMPLERS_USE_DATA_HALF=0",
    "-DFFX_FRAMEINTERPOLATION_OPTION_UPSAMPLE_USE_LANCZOS_TYPE=2",
    *COMMON_ARGUMENTS,
    "-DFFX_FRAMEINTERPOLATION_OPTION_LOW_RES_MOTION_VECTORS={0,1}",
    "-DFFX_FRAMEINTERPOLATION_OPTION_JITTER_MOTION_VECTORS={0,1}",
    "-DFFX_FRAMEINTERPOLATION_OPTION_INVERTED_DEPTH={0,1}",
]

OPTICAL_FLOW_ARGUMENTS = [*COMMON_ARGUMENTS, "-DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT={0,1}"]

OPTICAL_FLOW_PASSES = [
    "prepare_luma_pass",
    "compute_luminance_pyramid_pass",
    "generate_scd_histogram_pass",
    "compute_scd_divergence_pass",
    "compute_optical_flow_advanced_pass_v5",
    "filter_optical_flow_pass_v5",
    "scale_optical_flow_advanced_pass_v5",
]


def sha256_of(*paths):
    digest = hashlib.sha256()
    for path in paths:
        digest.update(Path(path).read_bytes())
    return digest.hexdigest()


def tool_path(path, runner):
    if not runner:
        return str(path)
    return "Z:" + str(path).replace("/", "\\")


def stage_sdk(sdk, staged):
    if staged.exists():
        shutil.rmtree(staged)
    for folder in STAGED_FOLDERS:
        shutil.copytree(sdk / folder, staged / folder)


def apply_patch(patch, staged):
    environment = dict(os.environ, GIT_CEILING_DIRECTORIES=str(staged.parent))
    subprocess.run(["git", "apply", "--whitespace=nowarn", str(patch)], cwd=staged, env=environment, check=True)
    marker_file, marker = FORK_MARKER
    if marker not in (staged / marker_file).read_text(encoding="utf-8", errors="replace"):
        raise SystemExit(f"{patch.name} did not reach {staged / marker_file}")


def compile_jobs(staged, shaders, runner, compiler):
    gpu = staged / "include/FidelityFX/gpu"
    backend = staged / "src/backends/dx12/shaders"
    components = [
        ("frameinterpolation", sorted((backend / "frameinterpolation").glob("*.hlsl")), FRAME_INTERPOLATION_ARGUMENTS),
        ("opticalflow", [backend / "opticalflow" / f"ffx_opticalflow_{name}.hlsl" for name in OPTICAL_FLOW_PASSES],
         OPTICAL_FLOW_ARGUMENTS),
    ]
    jobs = []
    for component, sources, arguments in components:
        for source in sources:
            for suffix, variant_arguments in VARIANTS:
                name = source.stem + suffix
                command = [*([runner] if runner else []), str(compiler), *arguments, f"-name={name}",
                           *variant_arguments,
                           "-I", tool_path(gpu, runner), "-I", tool_path(gpu / component, runner),
                           f"-output={tool_path(shaders, runner)}", tool_path(source, runner)]
                jobs.append((name, command))
    return jobs


def run_job(job, logs):
    name, command = job
    with open(logs / f"{name}.log", "wb") as log:
        result = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
    if result.returncode != 0:
        tail = (logs / f"{name}.log").read_text(encoding="utf-8", errors="replace").splitlines()[-20:]
        raise SystemExit(f"FidelityFX_SC failed on {name}\n" + "\n".join(tail))
    return name


def compile_shaders(jobs, logs, workers):
    with ThreadPoolExecutor(max_workers=workers) as pool:
        for _ in pool.map(lambda job: run_job(job, logs), jobs):
            pass


def up_to_date(stamp_file, stamp, shaders):
    return stamp_file.is_file() and stamp_file.read_text().strip() == stamp and any(shaders.glob("*_permutations.h"))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--sdk", type=Path, required=True, help="FidelityFX SDK v1.1.4 checkout (contains sdk/)")
    parser.add_argument("--patch", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--runner", default="", help="runs Windows executables (tools/linux/wine.sh); empty on Windows")
    parser.add_argument("--jobs", type=int, default=min(os.cpu_count() or 1, 8))
    arguments = parser.parse_args()

    sdk = arguments.sdk / "sdk"
    compiler = sdk / "tools/binary_store/FidelityFX_SC.exe"
    for required in (sdk / "include", compiler, arguments.patch):
        if not required.exists():
            raise SystemExit(f"missing {required}")

    output = arguments.output.resolve()
    staged = output / "sdk"
    shaders = output / "shaders"
    logs = output / "logs"
    stamp_file = output / "stamp.sha256"
    stamp = sha256_of(arguments.patch, Path(__file__), sdk / "src/components/frameinterpolation/ffx_frameinterpolation.cpp")
    if up_to_date(stamp_file, stamp, shaders):
        print(f"FSR shaders up to date ({len(list(shaders.glob('*_permutations.h')))} permutation headers)")
        return

    started = time.monotonic()
    if output.exists():
        shutil.rmtree(output)
    shaders.mkdir(parents=True)
    logs.mkdir()
    stage_sdk(sdk, staged)
    apply_patch(arguments.patch.resolve(), staged)
    runner = arguments.runner
    jobs = compile_jobs(staged, shaders, runner, compiler)
    compile_shaders(jobs, logs, arguments.jobs)
    headers = len(list(shaders.glob("*_permutations.h")))
    if headers == 0:
        raise SystemExit("FidelityFX_SC wrote no permutation headers")
    stamp_file.write_text(stamp + "\n")
    print(f"FSR shaders: {len(jobs)} compiles, {headers} permutation headers, {time.monotonic() - started:.0f} s")


if __name__ == "__main__":
    sys.exit(main())
