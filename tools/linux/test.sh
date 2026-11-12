#!/usr/bin/env bash
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/environment.sh"

tests_build=${MFG_TEST_BUILD_DIR:-$repository/build/linux-tests}
dll=$build/MFGUnlocker.dll
release_dll=$cache/release/v1.7.1/MFGUnlocker.dll
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
    [dlss5-replace-memory]="needs WARP, which Wine does not provide"
    [dlss5-auto-capacity]="needs WARP, which Wine does not provide"
    [dlss5-d3d11-out-of-date-fails-closed]="needs WARP, which Wine does not provide"
    [dlss5-d3d11-stage-revision]="needs WARP, which Wine does not provide"
    [dlss5-d3d11-depth-pass]="needs WARP; Wine's D3D11 falls back to a hardware device, where the depth pass's conversion is refused"
)

gpu_reason="needs MFG_TEST_GPU=1 (creates a GPU device)"

gpu_tests=(BlackwellAdapterProbe NvapiResolveTests XmxLatencyRuntimeTests XmxRuntimeDeviceTests
    XmxGeneratorReuseTests XmxGeneratorFramesTests XmxGeneratorRefusedTests XmxGeneratorRefusedAlternatingTests
    XmxGeneratorFallbackTests XmxGeneratorMemoryDeviceTests XmxGeneratorCopyMismatchTests)

gpu_source_level_cases=(optiscaler-streamline-device)

declare -A skipped_reasons=(
    [XmxRuntimeDeviceTests]="needs a D3D12 hardware device at feature level 12_0; Wine's vkd3d lists only a generic adapter and refuses 12_0"
)

regression_cases=(default-clean default-legacy default-fixed default-dynamic default-follow default-environment
    loader-version loader-winmm crash-reporter-host themida-imports status-transport
    policy dynamic-availability vulkan-menu-status gpuname-off gpuname-on
    gpuname-invalid small-stack-resolver log-retention interval-retention log-location session-diagnostics
    engine-direct engine-dynamic engine-late engine-middleware engine-external engine-conflict)

source_level_cases=(default-xmx default-xmx-standalone default-anr default-anr-xmx scoped-import scoped-factory scoped-factory-local-dxgi
    adapter-parent-local-dxgi control control-locked
    optiscaler-standalone optiscaler-proxy optiscaler-plugin optiscaler-late optiscaler-optin optiscaler-hidden
    optiscaler-shared optiscaler-forza optiscaler-forza-shared optiscaler-impostor optiscaler-fakenvapi
    optiscaler-streamline optiscaler-second-streamline optiscaler-streamline-device optiscaler-streamline-warp)

declare -A source_level_not_runnable=()

regression_targets=(SlInitFixture SlInitHotpatchFixture OptiScalerHostTests AdapterParentTests DxgiForwarderFixture StartupDefaultsTests ReleaseLoadCheck ThemidaImportHost ControlResolverTests
    ControlWrapperFixture StatusTransportTests ScopedImportTests ScopedFactoryTests ScopedCallerFixture
    NvidiaPolicyTests DynamicAvailabilityTests VulkanMenuStatusTests GpuNameStartupTests
    SmallStackResolverTests LogRetentionTests SessionDiagnosticsTests EngineFactoryTests
    EngineFactoryFixture ExistingOverlayFixture MaximumClampFixture OptiScalerHostFixture
    FakeNvapiFixture FactoryRevisionTests)

record() {
    printf '%s|%s|%s|%s\n' "$1" "$2" "$3" "$4" >>"$results"
}

gpu_gated() {
    [ "${MFG_TEST_GPU:-}" != 1 ]
}

gpu_exclusion() {
    local IFS='|'
    printf '^(%s)$' "${gpu_tests[*]}"
}

record_gpu_gated() {
    local project=$1 line name gated
    local pattern='Test +#[0-9]+: (.+)$'
    while IFS= read -r line; do
        if [[ $line =~ $pattern ]]; then
            name=${BASH_REMATCH[1]}
            for gated in "${gpu_tests[@]}"; do
                [ "$gated" = "$name" ] && record "$project" "$name" "not runnable" "$gpu_reason"
            done
        fi
    done < <(ctest --test-dir "$tests_build/$project" -N)
}

is_gpu_source_level_case() {
    local gated
    for gated in "${gpu_source_level_cases[@]}"; do
        [ "$gated" = "$1" ] && return 0
    done
    return 1
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
    local pattern='Test +#[0-9]+: ([^ ]+) \.+ *(.*[^ ]) +[0-9.]+ sec'
    local exclusion=()
    if gpu_gated; then
        exclusion=(-E "$(gpu_exclusion)")
        record_gpu_gated "$project"
    fi
    ctest --test-dir "$tests_build/$project" --timeout 120 --output-on-failure "${exclusion[@]}" >"$tests_build/$project/ctest.log" 2>&1
    while IFS= read -r line; do
        if [[ $line =~ $pattern ]]; then
            name=${BASH_REMATCH[1]}
            status=${BASH_REMATCH[2]}
            if [ "$status" = Passed ]; then
                record "$project" "$name" passed ""
            elif [ "$status" = "***Skipped" ]; then
                record "$project" "$name" "not runnable" "${skipped_reasons[$name]:-the test skipped itself; see $tests_build/$project/ctest.log}"
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
        record release_regression "$case_name" "not runnable" "fails the same way with the unmodified v1.7.1 DLL under Wine"
    else
        record release_regression "$case_name" failed "passes with the v1.7.1 DLL; $(tail -n 1 "$output/$case_name.log")"
    fi
}

run_source_level_case() {
    local case_name=$1 output=$tests_build/release_regression/results
    if gpu_gated && is_gpu_source_level_case "$case_name"; then
        record release_regression "$case_name" "not runnable" "$gpu_reason"
        return
    fi
    if regression_run "$dll" "$case_name" "$output"; then
        record release_regression "$case_name" passed ""
    elif [ -n "${source_level_not_runnable[$case_name]:-}" ]; then
        record release_regression "$case_name" "not runnable" "${source_level_not_runnable[$case_name]}"
    else
        record release_regression "$case_name" failed "$(tail -n 1 "$output/$case_name.log")"
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
        record release_regression FactoryRevisionTests failed "run build/linux-tests/release_regression/FactoryRevisionTests.exe under tools/linux/wine.sh for its output"
    fi
    for case_name in "${regression_cases[@]}"; do
        run_regression_case "$case_name"
    done
    for case_name in "${source_level_cases[@]}"; do
        run_source_level_case "$case_name"
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
run_project xmx -DSTREAMLINE_ROOT="${root[streamline]}" -DXESS_SDK_ROOT="${root[xess]}" \
    -DMFG_GENERATED_DIR="$build/generated" -DMFG_PAYLOAD_DLL="$dll" -DXMX_RUNTIME_DIR="${root[xess]}/bin"
run_project anr -DSTREAMLINE_ROOT="${root[streamline]}"
run_project dlss5 -DSTREAMLINE_ROOT="${root[streamline]}" -DDLSS5_CANDIDATE_DLL="$dll"
run_regression_project

print_summary
! grep -q '|failed|' "$results"
