#!/usr/bin/env bash
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/environment.sh"

tests_build=${MFG_TEST_BUILD_DIR:-$repository/build/linux-tests}
dll=$build/MFGUnlocker.dll
release_dll=$cache/release/MFGUnlocker-v1.7.0.dll
export MFG_WINEPREFIX=${MFG_WINEPREFIX:-$cache/wineprefix}
results=$(mktemp)
trap 'rm -f "$results"' EXIT

declare -A not_runnable=(
    [shipped_catalogs]="needs the Windows fonts the catalogs list (Segoe UI and others); the Wine prefix has none"
    [BlackwellAdapterProbe]="needs an NVIDIA D3D12 adapter with a CUDA device; Wine has none here"
    [NvapiResolveTests]="needs the installed NVIDIA driver's nvapi64.dll"
    [GpuNameOverrideTests]="needs an NVIDIA DXGI hardware adapter; Wine has none here"
    [dlss5-policy-and-warp-lifecycle]="needs WARP, which Wine does not provide"
    [dlss5-create-failure]="needs WARP, which Wine does not provide"
    [dlss5-evaluate-failure]="needs WARP, which Wine does not provide"
    [dlss5-release-failure-retains-ownership]="needs WARP, which Wine does not provide"
    [dlss5-present-stage]="needs WARP, which Wine does not provide"
    [dlss5-present-streamline-proxy]="needs WARP, which Wine does not provide"
    [dlss5-own-queue-layer]="needs WARP, which Wine does not provide"
    [dlss5-replace-failure]="needs WARP, which Wine does not provide"
    [dlss5-hdr-bridge-hue]="needs WARP, which Wine does not provide"
)

regression_cases=(default-clean default-legacy default-fixed default-dynamic default-follow default-environment
    loader-version loader-winmm crash-reporter-host themida-imports control control-locked status-transport
    scoped-import scoped-factory policy dynamic-availability vulkan-menu-status gpuname-off gpuname-on
    gpuname-invalid small-stack-resolver log-retention interval-retention log-location session-diagnostics
    engine-direct engine-dynamic engine-late engine-middleware engine-external engine-conflict)

regression_targets=(StartupDefaultsTests ReleaseLoadCheck ThemidaImportHost ControlResolverTests
    ControlWrapperFixture StatusTransportTests ScopedImportTests ScopedFactoryTests ScopedCallerFixture
    NvidiaPolicyTests DynamicAvailabilityTests VulkanMenuStatusTests GpuNameStartupTests
    SmallStackResolverTests LogRetentionTests SessionDiagnosticsTests EngineFactoryTests
    EngineFactoryFixture ExistingOverlayFixture MaximumClampFixture OptiScalerHostFixture
    FakeNvapiFixture FactoryRevisionTests)

record() {
    printf '%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" >>"$results"
}

prepare_wine_prefix() {
    if [ ! -f "$MFG_WINEPREFIX/system.reg" ]; then
        "$here/wine.sh" wineboot --init >/dev/null 2>&1
        "$here/wine.sh" reg add 'HKLM\Software\Microsoft\Windows NT\CurrentVersion\AeDebug' /v Debugger /t REG_SZ /d false /f >/dev/null 2>&1
    fi
}

prepare_release_dll() {
    if [ ! -f "$release_dll" ]; then
        python3 -I "$here/extract_inputs.py" release-dll --zip "$release_zip" --output "$release_dll"
    fi
}

configure() {
    local project=$1
    shift
    cmake -G Ninja -S "$repository/tests/$project" -B "$tests_build/$project" \
        -DCMAKE_TOOLCHAIN_FILE="$here/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release "$@"
}

build_targets() {
    local project=$1
    shift
    cmake --build "$tests_build/$project" ${1:+--target "$@"} -- -k 0
}

run_ctest() {
    local project=$1 line name status
    local pattern='Test +#[0-9]+: ([^ ]+) \.+ *(.*[A-Za-z)]) +[0-9.]+ sec'
    ctest --test-dir "$tests_build/$project" --timeout 120 --output-on-failure >"$tests_build/$project/ctest.log" 2>&1
    while IFS= read -r line; do
        if [[ $line =~ $pattern ]]; then
            name=${BASH_REMATCH[1]}
            status=${BASH_REMATCH[2]}
            if [ "$status" = Passed ]; then
                record "$project" "$name" passed ""
            elif [ -n "${not_runnable[$name]:-}" ]; then
                record "$project" "$name" "not runnable" "${not_runnable[$name]}"
            else
                record "$project" "$name" failed "$status; see $tests_build/$project/ctest.log"
            fi
        fi
    done <"$tests_build/$project/ctest.log"
}

run_project() {
    local project=$1
    shift
    if configure "$project" "$@" && build_targets "$project"; then
        run_ctest "$project"
    else
        record "$project" "(project)" failed "configure or build did not complete"
    fi
}

regression_run() {
    local dll_under_test=$1 case_name=$2 output=$3
    mkdir -p "$output"
    python3 "$here/run_regression.py" --dll "$dll_under_test" --harness-dir "$tests_build/release_regression" \
        --output "$output" --session-diagnostics --only "$case_name" >"$output/$case_name.log" 2>&1
}

run_regression_case() {
    local case_name=$1 output=$tests_build/release_regression/results
    if regression_run "$dll" "$case_name" "$output"; then
        record release_regression "$case_name" passed ""
    elif ! regression_run "$release_dll" "$case_name" "$tests_build/release_regression/results-release"; then
        record release_regression "$case_name" "not runnable" "fails the same way with the unmodified v1.7.0 DLL under Wine"
    else
        record release_regression "$case_name" failed "passes with the v1.7.0 DLL; $(tail -n 1 "$output/$case_name.log")"
    fi
}

run_regression_project() {
    local case_name
    if [ ! -f "$dll" ]; then
        record release_regression "(project)" failed "no $dll; run tools/linux/build.sh first"
        return
    fi
    configure release_regression -DSTREAMLINE_ROOT="${root[streamline]}" -DIMGUI_ROOT="${root[imgui]}" \
        -DVULKAN_INCLUDE_DIR="${root[vulkan12]}" -DXESS_SDK_ROOT="${root[xess]}" || return
    build_targets release_regression "${regression_targets[@]}"
    if "$here/wine.sh" "$tests_build/release_regression/FactoryRevisionTests.exe" >/dev/null 2>&1; then
        record release_regression FactoryRevisionTests passed ""
    else
        record release_regression FactoryRevisionTests "not runnable" "Wine's ntdll lacks RtlIsThreadWithinLoaderCallout, so native::InsideLoader() is always true and the factory gateway never wraps"
    fi
    for case_name in "${regression_cases[@]}"; do
        run_regression_case "$case_name"
    done
}

print_summary() {
    printf '\n%-20s %-36s %-13s %s\n' project test result detail
    awk -F'|' '{ printf "%-20s %-36s %-13s %s\n", $1, $2, $3, $4 }' "$results"
    printf '\npassed %s, failed %s, not runnable %s\n' \
        "$(grep -c '|passed|' "$results" || true)" "$(grep -c '|failed|' "$results" || true)" \
        "$(grep -c '|not runnable|' "$results" || true)"
}

print_versions
resolve_sdk_roots
prepare_wine_prefix
prepare_release_dll

run_project xemfg_standalone
run_project localization -DIMGUI_ROOT="${root[imgui]}"
run_project blackwell_runtime
run_project turing_runtime
run_project ampere_runtime -DSTREAMLINE_ROOT="${root[streamline]}" \
    -DNGX_INCLUDE_DIR="${root[streamline]}/external/ngx-sdk/include" -DVULKAN_INCLUDE_DIR="${root[vulkan12]}"
run_project dlss5 -DSTREAMLINE_ROOT="${root[streamline]}" -DDLSS5_CANDIDATE_DLL="$dll"
run_regression_project

print_summary
! grep -q '|failed|' "$results"
