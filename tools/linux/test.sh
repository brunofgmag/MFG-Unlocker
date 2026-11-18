#!/usr/bin/env bash
set -uo pipefail
set -m

source "$(dirname "${BASH_SOURCE[0]}")/environment.sh"
[ -n "${MFG_HEADLESS_ACTIVE:-}" ] || exec "$here/headless.sh" "$here/test.sh" "$@"

tests_build=${MFG_TEST_BUILD_DIR:-$repository/build/linux-tests}
dll=$build/MFGUnlocker.dll
release_dll=$cache/release/v1.7.1/MFGUnlocker.dll
export MFG_WINEPREFIX=${MFG_WINEPREFIX:-$cache/wineprefix}
jobs=${MFG_TEST_JOBS:-8}
results=$(mktemp)
outcomes=$tests_build/release_regression/outcomes

stop_wineserver() {
    WINEPREFIX=$MFG_WINEPREFIX wineserver -k >/dev/null 2>&1
    WINEPREFIX=$MFG_WINEPREFIX wineserver -w >/dev/null 2>&1
}

stop_background_jobs() {
    local pid
    for pid in $(jobs -p); do
        kill -TERM -- "-$pid" 2>/dev/null
    done
}

finish() {
    trap - EXIT INT TERM
    stop_background_jobs
    rm -f "$results"
    stop_wineserver
}
trap finish EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

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

gpu_disabled_reason="needs MFG_TEST_GPU=1 (creates a GPU device)"
gpu_reason=$gpu_disabled_reason

gpu_tests=(BlackwellAdapterProbe NvapiResolveTests XmxLatencyRuntimeTests XmxRuntimeDeviceTests
    XmxGeneratorReuseTests XmxGeneratorFramesTests XmxGeneratorRefusedTests XmxGeneratorRefusedAlternatingTests
    XmxGeneratorFallbackTests XmxGeneratorMemoryDeviceTests XmxGeneratorCopyMismatchTests
    XmxGeneratorHudCompositeTests XmxGeneratorHudExecuteTests XmxGeneratorHudDeviceTests XmxGeneratorHudGeneratedTests
    XmxGeneratorHudUnpreparedTests XmxGeneratorTensorCoresTests XmxGeneratorModelDefaultTests
    XmxGeneratorModelFsrTests XmxGeneratorModelFsrMissingTests XmxGeneratorModelLatchedTests)

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
    local status
    gpu_reason=$gpu_disabled_reason
    [ "${MFG_TEST_GPU:-}" = 1 ] || return 0
    mkdir -p "$tests_build"
    "$here/proton_cuda.sh" game-closed >"$tests_build/game_check.log" 2>&1
    status=$?
    [ "$status" = 3 ] || return 1
    gpu_reason="a game is open; see $tests_build/game_check.log"
    return 0
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

keep_wineserver() {
    stop_wineserver
    WINEPREFIX=$MFG_WINEPREFIX wineserver -p </dev/null >/dev/null 2>&1 &
    "$here/wine.sh" wineboot </dev/null >/dev/null 2>&1
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

record_ctest_result() {
    local project=$1 name=$2 status=$3
    if [ "$status" = Passed ]; then
        record "$project" "$name" passed ""
    elif [ "$status" = "***Skipped" ]; then
        record "$project" "$name" "not runnable" "${skipped_reasons[$name]:-the test skipped itself; see $tests_build/$project/ctest.log}"
    elif [ -n "${not_runnable[$name]:-}" ]; then
        record "$project" "$name" "not runnable" "${not_runnable[$name]}"
    else
        record "$project" "$name" failed "$status; see $tests_build/$project/ctest.log"
    fi
}

run_ctest() {
    local project=$1 line log=$tests_build/$project/ctest.log
    local pattern='Test +#[0-9]+: ([^ ]+) \.+ *(.*[^ ]) +[0-9.]+ sec'
    local listing='Test +#[0-9]+: (.+)$'
    local -A statuses=()
    ctest --test-dir "$tests_build/$project" -j "$jobs" --timeout 120 --output-on-failure -E "$(gpu_exclusion)" >"$log" 2>&1
    if gpu_gated; then
        record_gpu_gated "$project"
    else
        ctest --test-dir "$tests_build/$project" --timeout 120 --output-on-failure -R "$(gpu_exclusion)" >>"$log" 2>&1
    fi
    while IFS= read -r line; do
        [[ $line =~ $pattern ]] && statuses[${BASH_REMATCH[1]}]=${BASH_REMATCH[2]}
    done <"$log"
    while IFS= read -r line; do
        [[ $line =~ $listing ]] && [ -n "${statuses[${BASH_REMATCH[1]}]:-}" ] &&
            record_ctest_result "$project" "${BASH_REMATCH[1]}" "${statuses[${BASH_REMATCH[1]}]}"
    done < <(ctest --test-dir "$tests_build/$project" -N)
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
    local case_name=$1 output=$tests_build/release_regression/results/$1
    if regression_run "$dll" "$case_name" "$output"; then
        record release_regression "$case_name" passed ""
    elif ! regression_run "$release_dll" "$case_name" "$tests_build/release_regression/results-release/$case_name"; then
        record release_regression "$case_name" "not runnable" "fails the same way with the unmodified v1.7.1 DLL under Wine"
    else
        record release_regression "$case_name" failed "passes with the v1.7.1 DLL; $(tail -n 1 "$output/$case_name.log")"
    fi
}

run_source_level_case() {
    local case_name=$1 output=$tests_build/release_regression/results/$1
    if is_gpu_source_level_case "$case_name" && gpu_gated; then
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

running_pids() {
    local pid
    for pid in "$@"; do
        kill -0 "$pid" 2>/dev/null && printf '%s\n' "$pid"
    done
}

run_cases_in_parallel() {
    local entry runner case_name pid
    local pids=()
    for entry in "$@"; do
        read -r runner case_name <<<"$entry"
        while [ "${#pids[@]}" -ge "$jobs" ]; do
            wait -n "${pids[@]}"
            pids=($(running_pids "${pids[@]}"))
        done
        ( results=$outcomes/$case_name; "$runner" "$case_name" ) </dev/null &
        pids+=("$!")
    done
    for pid in "${pids[@]}"; do
        wait "$pid"
    done
}

record_outcome() {
    if [ -f "$outcomes/$1" ]; then
        cat "$outcomes/$1" >>"$results"
    else
        record release_regression "$1" failed "no outcome was written; see $tests_build/release_regression/results/$1/$1.log"
    fi
}

record_factory_revision_test() {
    if "$here/wine.sh" "$tests_build/release_regression/FactoryRevisionTests.exe" >/dev/null 2>&1; then
        record release_regression FactoryRevisionTests passed ""
    else
        record release_regression FactoryRevisionTests failed "run build/linux-tests/release_regression/FactoryRevisionTests.exe under tools/linux/wine.sh for its output"
    fi
}

run_regression_cases() {
    local case_name
    local lanes=()
    rm -rf "$outcomes" "$tests_build/release_regression/results" "$tests_build/release_regression/results-release"
    mkdir -p "$outcomes"
    for case_name in "${regression_cases[@]}"; do
        lanes+=("run_regression_case $case_name")
    done
    for case_name in "${source_level_cases[@]}"; do
        lanes+=("run_source_level_case $case_name")
    done
    run_cases_in_parallel "${lanes[@]}"
    for case_name in "${regression_cases[@]}" "${source_level_cases[@]}"; do
        record_outcome "$case_name"
    done
}

run_regression_project() {
    if [ ! -f "$dll" ]; then
        record release_regression "(project)" failed "no $dll; run tools/linux/build.sh first"
        return
    fi
    configure release_regression -DSTREAMLINE_ROOT="${root[streamline]}" -DIMGUI_ROOT="${root[imgui]}" \
        -DVULKAN_INCLUDE_DIR="${root[vulkan12]}" -DXESS_SDK_ROOT="${root[xess]}" || return
    build_targets release_regression "${regression_targets[@]}"
    record_factory_revision_test
    run_regression_cases
}

proton_results=${MFG_PROTON_CUDA_OUT:-$repository/build/proton-cuda}/route/results.txt

run_proton_project() {
    local log=$tests_build/proton_route.log status
    mkdir -p "$tests_build"
    rm -f "$proton_results"
    "$here/proton_cuda.sh" route >"$log" 2>&1
    status=$?
    [ -f "$proton_results" ] && cat "$proton_results" >>"$results"
    if [ "$status" = 3 ]; then
        record xess_cuda_proton "(project)" "not runnable" "a game is open; see $log"
    elif [ ! -f "$proton_results" ]; then
        record xess_cuda_proton "(project)" failed "proton_cuda.sh route stopped before any test ran (exit $status); see $log"
    fi
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
keep_wineserver
prepare_release_dll

run_project xemfg_standalone
run_project localization -DIMGUI_ROOT="${root[imgui]}"
run_project blackwell_runtime
run_project turing_runtime
run_project ampere_runtime -DSTREAMLINE_ROOT="${root[streamline]}" \
    -DNGX_INCLUDE_DIR="${root[streamline]}/external/ngx-sdk/include" -DVULKAN_INCLUDE_DIR="${root[vulkan12]}"
run_project xmx -DSTREAMLINE_ROOT="${root[streamline]}" -DXESS_SDK_ROOT="${root[xess]}" \
    -DMFG_GENERATED_DIR="$build/generated" -DMFG_PAYLOAD_DLL="$dll" -DXMX_RUNTIME_DIR="${root[xess]}/bin"
run_project fsr -DMFG_GENERATED_DIR="$build/generated"
run_project anr -DSTREAMLINE_ROOT="${root[streamline]}"
run_project dlss5 -DSTREAMLINE_ROOT="${root[streamline]}" -DDLSS5_CANDIDATE_DLL="$dll"
run_regression_project
[ "${MFG_TEST_PROTON:-}" = 1 ] && run_proton_project

print_summary
! grep -q '|failed|' "$results"
