#!/usr/bin/env bash
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/environment.sh"

print_versions
resolve_sdk_roots
ensure_inputs

cmake -G Ninja -S "$native" -B "$build" \
    -DCMAKE_TOOLCHAIN_FILE="$here/toolchain.cmake" \
    -DCMAKE_BUILD_TYPE=Release \
    -DMFG_PREGENERATED_INPUTS="$inputs/pregenerated" \
    -DSTREAMLINE_ROOT="${root[streamline]}" \
    -DIMGUI_ROOT="${root[imgui]}" \
    -DMFG_UNLOCK_XESS=ON \
    -DXESS_SDK_ROOT="${root[xess]}" \
    -DXESS_PROGRAMS_ROOT="$inputs/xess-programs" \
    -DXVK_VULKAN_INCLUDE_DIR="${root[vulkan14]}" \
    -DDLSS5_VK_DXC="$here/dxc" \
    -DVULKAN_INCLUDE_DIR="${root[vulkan12]}" \
    -DMFG_UNLOCK_BUILD_SINGLE_MODULE=ON \
    -DMFG_UNLOCK_OVERLAY_SKIP_GPU_WORK=OFF \
    -DMFG_UNLOCK_NGX_CREATE_RESULT_DIAGNOSTICS=ON \
    -DMFG_UNLOCK_GPU_FAULT_CAPTURE=OFF \
    -DMFG_UNLOCK_SESSION_DIAGNOSTICS=ON \
    -DMFG_UNLOCK_XVK_CAPTURE=OFF \
    -DMFG_UNLOCK_XEFG_VK_STANDALONE=OFF \
    -DMFG_UNLOCK_DIAGNOSTIC_NO_SINGLE_OVERLAY=OFF \
    -DMFG_UNLOCK_BOUNDED_OVERLAY_REDESIGN=ON \
    -DMFG_UNLOCK_OVERLAY_MENU_DRAW=ON \
    -DMFG_UNLOCK_OVERLAY_TEST_FAULTS=OFF \
    -DMFG_UNLOCK_OUTPUT_PULL_MASK_ONLY=ON \
    -DMFG_UNLOCK_OUTPUT_PULL_MASK_OCCUPANCY=OFF \
    -DMFG_AMPERE_EMBEDDED_KERNELS=ON \
    -DMFG_AMPERE_KERNEL_IMAGE=AUTO \
    -DMFG_AMPERE_NATIVE_MANIFEST="$inputs/native-cache/ampere_native_manifest.inc" \
    -DMFG_AMPERE_KERNEL_DIRECTORY="$inputs/kernels" \
    -DMFG_AMPERE_NATIVE_MANIFEST_3109="$inputs/native-cache-3109/ampere_native_manifest.inc" \
    -DMFG_AMPERE_KERNEL_DIRECTORY_3109="$inputs/kernels" \
    -DMFG_AMPERE_NATIVE_MANIFEST_ADDITIONAL="$inputs/native-cache-additional/ampere_native_manifest_additional.inc" \
    -DMFG_AMPERE_NATIVE_RESOURCES_ADDITIONAL="$inputs/native-cache-additional/additional-resources.json" \
    -DMFG_AMPERE_KERNEL_DIRECTORY_ADDITIONAL="$inputs/kernels"

cmake --build "$build" --target RTXMFGUnified

ls -l "$build/MFGUnlocker.dll"
sha256sum "$build/MFGUnlocker.dll"
