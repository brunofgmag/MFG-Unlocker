#!/usr/bin/env bash
set -uo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/environment.sh"
[ -n "${MFG_HEADLESS_ACTIVE:-}" ] || [ "${1:-}" = game-closed ] || exec "$here/headless.sh" "$here/proton_cuda.sh" "$@"

nvcc=${MFG_NVCC:-/opt/cuda/bin/nvcc}
proton=${MFG_PROTON:-$HOME/.steam/steam/compatibilitytools.d/Proton-FS2024/proton}
runtime=${MFG_STEAM_RUNTIME:-$HOME/Jogos/SteamLibrary/steamapps/common/SteamLinuxRuntime_4/_v2-entry-point}
reshade_dll=${MFG_RESHADE_DLL:-$HOME/Jogos/SteamLibrary/steamapps/common/The Witcher 3/bin/x64_dx12/dxgi.dll}
reshade_sha256=0cee63f9c9f13f3ac909c5b4903f4dbb4b719a7ab3b4f13b0deaf83c814b94f7
game_dir=${MFG_GAME_DIR:-$HOME/Jogos/SteamLibrary/steamapps/common/MSFS2024}
scskiller_sha256=0aee5c65daf32108c1355b85eb030321da92425a3c1b97b0c44a09f6206e0a69
addons=(
    AtmosphereMod.addon64:05f96ca43339cee9f55551560e2a69c4ac3a27e345812b5b3a6d964fa758d384
    glass-cockpit-dlss-fix.addon64:411cfa6f51f8418860d85c8e9a6a10cfbc30b361d8c3115eb0c896dddfb31547
    renodx-msfs2024.addon64:304008e72d2fa6a9efe23bea527dec79525ae82decd7c16ef0c36bb27eeecb7e
)
modes=(plain reshade addons scskiller game)
route_modes=(route intel-programs)
default_vkd3d_config=no_upload_hvv,no_gpu_upload_heap
prefix=${MFG_PROTON_CUDA_PREFIX:-$cache/proton-cuda-prefix}
out=${MFG_PROTON_CUDA_OUT:-$repository/build/proton-cuda}
tests_build=${MFG_TEST_BUILD_DIR:-$repository/build/linux-tests}
project_build=$tests_build/proton_cuda
exe_name=proton_cuda_probe.exe
route_build=$tests_build/xess_cuda
route_timeout=${MFG_ROUTE_TIMEOUT:-900}
intel_dir=$out/intel-programs
intel_programs=${MFG_INTEL_PROGRAMS:-$cache/intel-programs}
layer_table=$repository/source/native/xess/cuda/cuda_layers.h
intel_seconds=${MFG_INTEL_DUMP_SECONDS:-90}
proton_wineserver=$(dirname "$proton")/files/bin/wineserver

usage() {
    printf 'usage: %s [build|plain|reshade|addons|scskiller|game|all|route|intel-programs|profile|game-closed]\n' "$(basename "$0")" >&2
    exit 2
}

build_cubin() {
    mkdir -p "$out"
    "$nvcc" -cubin -arch=sm_89 -o "$out/probe_kernels.cubin" "$repository/tests/proton_cuda/probe_kernels.cu"
}

build_exe() {
    cmake -G Ninja -S "$repository/tests/proton_cuda" -B "$project_build" \
        -DCMAKE_TOOLCHAIN_FILE="$here/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
    && cmake --build "$project_build" -- -k 0
}

mapping_game() {
    local pid=$1 line
    while IFS= read -r line; do
        case $line in
            *steamapps/common/SteamLinuxRuntime*|*steamapps/common/Proton*) ;;
            *) printf '%s\n' "$line"; return 0 ;;
        esac
    done < <(grep -hF -e 'steamapps/common/' -e "$HOME/Jogos/Heroic/" "/proc/$pid/maps" 2>/dev/null)
    return 1
}

game_is_open() {
    local p hit open=1 unreadable=0
    for p in /proc/[0-9]*; do
        if [ ! -r "$p/maps" ]; then unreadable=$((unreadable + 1)); continue; fi
        if hit=$(mapping_game "${p#/proc/}") && [ -n "$hit" ]; then
            printf 'REFUSED: pid %s maps %s\n' "${p#/proc/}" "$hit" >&2
            open=0
        fi
    done
    printf 'game check: %s process maps unreadable, game found among the readable ones: %s\n' \
        "$unreadable" "$([ "$open" = 0 ] && echo yes || echo no)" >&2
    return $open
}

windows_path() {
    printf 'Z:%s' "$(printf '%s' "$1" | sed 's|/|\\|g')"
}

copy_verified() {
    local source=$1 target=$2 expected=$3 before after copied
    before=$(sha256sum "$source" | cut -d' ' -f1)
    if [ "$before" != "$expected" ]; then
        printf '%s sha256 is %s, expected %s\n' "$source" "$before" "$expected" >&2
        return 1
    fi
    cp "$source" "$target" || return 1
    copied=$(sha256sum "$target" | cut -d' ' -f1)
    after=$(sha256sum "$source" | cut -d' ' -f1)
    printf 'sha256 %s before=%s copy=%s after=%s\n' "$(basename "$source")" "$before" "$copied" "$after" | tee -a "$out/$mode.hashes"
    [ "$copied" = "$expected" ] && [ "$after" = "$expected" ]
}

mode_uses() {
    case $1:$mode in
        reshade:reshade|reshade:addons|reshade:game) return 0 ;;
        addons:addons|addons:game) return 0 ;;
        scskiller:scskiller|scskiller:game) return 0 ;;
    esac
    return 1
}

stage() {
    local mode=$1 dir=$out/$1 entry
    rm -rf "$dir"
    mkdir -p "$dir/logs"
    rm -f "$out/$mode.hashes"
    cp "$project_build/$exe_name" "$out/probe_kernels.cubin" "$dir/"
    if mode_uses reshade; then
        copy_verified "$reshade_dll" "$dir/dxgi.dll" "$reshade_sha256" || return 1
    fi
    if mode_uses addons; then
        for entry in "${addons[@]}"; do
            copy_verified "$game_dir/${entry%%:*}" "$dir/${entry%%:*}" "${entry##*:}" || return 1
        done
    fi
    if mode_uses scskiller; then
        copy_verified "$game_dir/d3d12.dll" "$dir/d3d12.dll" "$scskiller_sha256" || return 1
    fi
    [ -f "$out/$mode.hashes" ] && cp "$out/$mode.hashes" "$dir/hashes.txt"
    return 0
}

dll_overrides() {
    local parts=()
    mode_uses reshade && parts+=("dxgi=n,b")
    mode_uses scskiller && parts+=("d3d12=n")
    local IFS=';'
    printf '%s' "${parts[*]}"
}

proton_environment() {
    local dir=$1 logs=$2 mounts=${3:-$1} vkd3d_config=${MFG_PROBE_VKD3D_CONFIG-$default_vkd3d_config}
    mkdir -p "$prefix"
    export STEAM_COMPAT_DATA_PATH=$prefix
    export STEAM_COMPAT_CLIENT_INSTALL_PATH=$HOME/.steam/steam
    export STEAM_COMPAT_INSTALL_PATH=$dir
    export STEAM_COMPAT_MOUNTS=$mounts
    export PRESSURE_VESSEL_FILESYSTEMS_RW=$mounts:$prefix
    export PROTONFIXES_DISABLE=1
    export PROTON_NVIDIA_NVCUDA=1
    export DXVK_NVAPI_LOG_LEVEL=info
    export DXVK_NVAPI_LOG_PATH
    DXVK_NVAPI_LOG_PATH=$(windows_path "$logs")
    export VKD3D_DEBUG=warn
    export VKD3D_LOG_FILE
    VKD3D_LOG_FILE=$(windows_path "$logs/vkd3d.log")
    mkdir -p "$out/shader-cache"
    export VKD3D_SHADER_CACHE_PATH
    VKD3D_SHADER_CACHE_PATH=$(windows_path "$out/shader-cache")
    unset WINEDLLOVERRIDES VKD3D_CONFIG
    [ -n "$vkd3d_config" ] && export VKD3D_CONFIG=$vkd3d_config
    return 0
}

run_mode() {
    local mode=$1 dir=$out/$1 results overrides rc
    stage "$mode" || return 1
    results=$dir/results.txt
    proton_environment "$dir" "$dir/logs"
    overrides=$(dll_overrides)
    [ -n "$overrides" ] && export WINEDLLOVERRIDES=$overrides
    printf 'mode %s: WINEDLLOVERRIDES=%s VKD3D_CONFIG=%s\n' "$mode" "${WINEDLLOVERRIDES:-}" "${VKD3D_CONFIG:-}"
    if game_is_open; then
        printf 'a game is open: %s run not started\n' "$mode" >&2
        return 3
    fi
    timeout 900 "$runtime" --verb=waitforexitandrun -- "$proton" waitforexitandrun "$dir/$exe_name" \
        "$(windows_path "$results")" >"$dir/proton.log" 2>&1
    rc=$?
    printf 'proton exit=%s\n' "$rc"
    printf '=== %s: results ===\n' "$mode"
    if [ -f "$results" ]; then cat "$results"; else printf '(no results file; tail of proton.log follows)\n'; tail -n 30 "$dir/proton.log"; fi
    printf '=== %s: logs in %s ===\n' "$mode" "$dir/logs"
    return $rc
}

linux_fatbin() {
    local output=$1
    python3 -I "$repository/source/native/xess/cuda/generate_cuda_layers.py" --linux --nvcc "$nvcc" \
        --source "$repository/source/native" --output "$output" >&2 && printf '%s' "$output/xess_cuda/xefg_cuda_layers.fatbin"
}

route_addon_entries() {
    local entry
    for entry in "${addons[@]}"; do
        case $entry in
            glass-cockpit*) ;;
            *) printf '%s\n' "$entry" ;;
        esac
    done
}

stop_proton_wineserver() {
    WINEPREFIX=$prefix/pfx "$proton_wineserver" -k >/dev/null 2>&1
    WINEPREFIX=$prefix/pfx "$proton_wineserver" -w >/dev/null 2>&1
}

dump_intel_programs() {
    local dir=$intel_dir
    rm -rf "$dir"
    mkdir -p "$dir/sdk" "$dir/dump" "$dir/logs"
    cp -r "$xess/bin/." "$dir/sdk/" || return 1
    proton_environment "$dir" "$dir/logs"
    export VKD3D_SHADER_DUMP_PATH
    VKD3D_SHADER_DUMP_PATH=$(windows_path "$dir/dump")
    if game_is_open; then
        printf 'a game is open: the Intel program dump was not started\n' >&2
        return 3
    fi
    (cd "$dir/sdk" && timeout "$intel_seconds" "$runtime" --verb=waitforexitandrun -- "$proton" waitforexitandrun \
        "$(windows_path "$dir/sdk/basic_sample_frame_generation.exe")" >"$dir/proton.log" 2>&1)
    printf 'sample exit=%s\n' "$?"
    stop_proton_wineserver
    printf '%s dumped files\n' "$(find "$dir/dump" -type f | wc -l)"
    collect_intel_programs "$dir/dump"
}

layer_shas() {
    grep -oE '"[0-9a-f]{64}"' "$layer_table" | tr -d '"' | sort -u
}

swap_in_intel_programs() {
    local staged=$1
    rm -rf "$intel_programs.old"
    [ -d "$intel_programs" ] && mv "$intel_programs" "$intel_programs.old"
    mv "$staged" "$intel_programs" || return 1
    rm -rf "$intel_programs.old"
}

collect_intel_programs() {
    local dump=$1 file sha wanted staged missing
    wanted=$(layer_shas)
    mkdir -p "$(dirname "$intel_programs")"
    staged=$(mktemp -d "$intel_programs.new.XXXXXX") || return 1
    for file in "$dump"/*.dxil "$dump"/*.dxbc; do
        [ -f "$file" ] || continue
        sha=$(sha256sum "$file" | cut -d' ' -f1)
        grep -qx "$sha" <<<"$wanted" && cp "$file" "$staged/$sha.dxil"
    done
    missing=$(missing_layer_programs "$staged")
    if [ -n "$missing" ]; then
        printf 'the dump lacks layer programs, the existing cache in %s stays:\n%s\n' "$intel_programs" "$missing" >&2
        rm -rf "$staged"
        return 1
    fi
    swap_in_intel_programs "$staged" || return 1
    printf '%s layer programs kept in %s\n' "$(find "$intel_programs" -type f | wc -l)" "$intel_programs"
}

missing_layer_programs() {
    local have programs=${1:-$intel_programs}
    have=$({ sha256sum "$programs"/*.dxil "$inputs"/xess-programs/programs/*.dxil "$inputs"/pregenerated/xess_shaders/*.dxil; } 2>/dev/null | cut -d' ' -f1 | sort -u)
    comm -23 <(layer_shas) <(printf '%s\n' "$have")
}

ensure_intel_programs() {
    local missing
    missing=$(missing_layer_programs)
    [ -z "$missing" ] && return 0
    printf 'layer programs missing, dumping them from libxess_fg %s under Proton:\n%s\n' "$(basename "$xess")" "$missing"
    dump_intel_programs || return $?
    missing=$(missing_layer_programs)
    [ -z "$missing" ] || { printf 'still missing after the dump:\n%s\n' "$missing" >&2; return 1; }
}

stage_route() {
    local mode=$1 dir=$2 fatbin entry
    rm -rf "$dir"
    mkdir -p "$dir/inputs" "$dir/reshade" "$dir/tests"
    rm -f "$out/$mode.hashes"
    fatbin=$(linux_fatbin "$dir/fatbin") || return 1
    cp "$fatbin" "$dir/inputs/xefg_cuda_layers.fatbin" || return 1
    cp -r "$inputs/xess-programs/programs" "$dir/inputs/programs" || return 1
    cp -r "$intel_programs" "$dir/inputs/intel" || return 1
    cp -r "$inputs/pregenerated/xess_shaders" "$dir/inputs/shaders" || return 1
    printf 'sha256 xefg_cuda_layers.fatbin %s (%s)\n' "$(sha256sum "$dir/inputs/xefg_cuda_layers.fatbin" | cut -d' ' -f1)" "$mode" | tee -a "$out/$mode.hashes"
    copy_verified "$reshade_dll" "$dir/reshade/dxgi.dll" "$reshade_sha256" || return 1
    while IFS= read -r entry; do
        copy_verified "$game_dir/${entry%%:*}" "$dir/reshade/${entry%%:*}" "${entry##*:}" || return 1
    done < <(route_addon_entries)
    cp "$out/$mode.hashes" "$dir/hashes.txt"
}

route_addon_paths() {
    local dir=$1 entry paths=()
    while IFS= read -r entry; do
        paths+=("$dir/reshade/${entry%%:*}")
    done < <(route_addon_entries)
    local IFS=';'
    printf '%s' "${paths[*]}"
}

build_route() {
    local dir=$1
    rm -rf "$route_build/reshade"
    cmake -G Ninja -S "$repository/tests/xess_cuda" -B "$route_build" \
        -DCMAKE_TOOLCHAIN_FILE="$here/toolchain.cmake" -DCMAKE_BUILD_TYPE=Release \
        -DXEFG_CUDA_FATBIN="$dir/inputs/xefg_cuda_layers.fatbin" -DXEFG_NATIVE_PROGRAMS="$dir/inputs/intel" \
        -DXEFG_PORTED_PROGRAMS="$dir/inputs/programs" -DXEFG_OWN_SHADERS="$dir/inputs/shaders" \
        -DXEFG_RESHADE_DXGI="$dir/reshade/dxgi.dll" "-DXEFG_RESHADE_ADDONS=$(route_addon_paths "$dir")" \
    && cmake --build "$route_build" -- -k 0
}

check_built_copy() {
    local hashes=$1 path=$2 expected=$3 actual
    actual=$(sha256sum "$path" | cut -d' ' -f1)
    printf 'sha256 %s built-copy=%s\n' "$(basename "$path")" "$actual" | tee -a "$hashes"
    [ "$actual" = "$expected" ]
}

check_reshade_copies() {
    local hashes=$out/$1.hashes entry
    check_built_copy "$hashes" "$route_build/reshade/dxgi.dll" "$reshade_sha256" || return 1
    while IFS= read -r entry; do
        check_built_copy "$hashes" "$route_build/reshade/${entry%%:*}" "${entry##*:}" || return 1
    done < <(route_addon_entries)
}

windows_arguments() {
    local argument
    for argument in "$@"; do
        case $argument in
            /*) windows_path "$argument" ;;
            *) printf '%s' "$argument" ;;
        esac
        printf '\n'
    done
}

route_detail() {
    local log=$1 line
    line=$(grep -a -E '^[0-9]+ cases, |^error:|^usage' "$log" | tail -n 1)
    [ -n "$line" ] || line=$(grep -a -v -E '^(EXIT |Unable to read VR)' "$log" | tail -n 1)
    printf '%s' "${line//$'\r'/}"
}

test_exit_code() {
    local log=$1 line
    line=$(grep -a -E '^EXIT [0-9]+' "$log" | tail -n 1)
    line=${line#EXIT }
    printf '%s' "${line//[[:space:]]/}"
}

record_route_result() {
    local results=$1 name=$2 rc=$3 log=$4 detail
    if [ "$rc" = 0 ]; then
        printf 'xess_cuda_proton|%s|passed|\n' "$name" >>"$results"
        return 0
    fi
    detail=$(route_detail "$log")
    printf 'xess_cuda_proton|%s|failed|exit %s: %s; see %s\n' "$name" "${rc:-none}" "${detail:-no output}" "$log" >>"$results"
}

run_route_test() {
    local dir=$1 name=$2 exe=$3 arguments=() test_dir rc
    shift 3
    test_dir=$dir/tests/$name
    mkdir -p "$test_dir/logs"
    mapfile -t arguments < <(windows_arguments "$@")
    proton_environment "$route_build" "$test_dir/logs" "$route_build:$dir"
    case $name in *[Rr]e[Ss]hade*) export WINEDLLOVERRIDES=dxgi=n,b ;; esac
    if game_is_open; then
        printf 'a game is open: %s not started\n' "$name" >&2
        return 3
    fi
    timeout "$route_timeout" "$runtime" --verb=waitforexitandrun -- "$proton" waitforexitandrun \
        cmd /v:on /c "$(windows_path "$exe")" "${arguments[@]}" ">$(windows_path "$test_dir/output.txt")" "2>&1" \
        "&" echo EXIT "!ERRORLEVEL!>>$(windows_path "$test_dir/output.txt")" >"$test_dir/proton.log" 2>&1
    rc=$(test_exit_code "$test_dir/output.txt")
    printf '%s exit=%s\n' "$name" "${rc:-none}"
    record_route_result "$dir/results.txt" "$name" "$rc" "$test_dir/output.txt"
    return 0
}

run_route_tests() {
    local dir=$1 line name command=() count=0 filter=${MFG_ROUTE_FILTER:-.}
    while IFS= read -r line; do
        count=$((count + 1))
        name=$(jq -r .name <<<"$line")
        mapfile -t command < <(jq -r '.command[]' <<<"$line")
        [ "${command[0]}" = "$here/wine.sh" ] && command=("${command[@]:1}")
        run_route_test "$dir" "$name" "${command[@]}" || return $?
    done < <(ctest --test-dir "$route_build" -R "$filter" --show-only=json-v1 | jq -c '.tests[]')
    if [ "$count" = 0 ]; then
        printf 'error: no ctest case matches the filter %s\n' "$filter" >&2
        printf 'xess_cuda_proton|(selection)|failed|no ctest case matches the filter %s\n' "$filter" >>"$dir/results.txt"
        return 1
    fi
}

run_xmx_contexts_test() {
    local dir=$1 name=XmxTensorCoreContextsTests exe=$tests_build/xmx/XmxTensorCoreContextsTests.exe
    [[ $name =~ ${MFG_ROUTE_FILTER:-.} ]] || return 0
    if [ ! -f "$exe" ]; then
        printf 'xess_cuda_proton|%s|failed|%s is not built; tools/linux/test.sh builds tests/xmx\n' "$name" "$exe" >>"$dir/results.txt"
        return 0
    fi
    ( export MFG_TEST_GPU=1; run_route_test "$dir" "$name" "$exe" "$xess/bin" "$build/MFGUnlocker.dll" )
}

stage_profile_reshade() {
    local dir=$1 exe=$2 entry
    rm -rf "$dir/bin"
    mkdir -p "$dir/bin"
    cp "$exe" "$dir/bin/" || return 1
    copy_verified "$reshade_dll" "$dir/bin/dxgi.dll" "$reshade_sha256" || return 1
    while IFS= read -r entry; do
        copy_verified "$game_dir/${entry%%:*}" "$dir/bin/${entry%%:*}" "${entry##*:}" || return 1
    done < <(route_addon_entries)
    printf '[GENERAL]\nCheckForUpdates=0\n[OVERLAY]\nTutorialProgress=4\n' >"$dir/bin/ReShade.ini"
}

profile_reshade_header() {
    local entry names=()
    while IFS= read -r entry; do
        names+=("${entry%%:*}")
    done < <(route_addon_entries)
    printf 'profile under ReShade (dxgi.dll %s) with the add-ons %s; glass-cockpit-dlss-fix.addon64 is not staged, as in the route tests\n' \
        "$reshade_sha256" "${names[*]}"
}

name_profile_programs() {
    local output=$1 dir=$2 known word number sha path
    known=$({ sha256sum "$inputs"/xess-programs/programs/*.dxil "$inputs"/pregenerated/xess_shaders/*.dxil "$intel_programs"/*.dxil; } 2>/dev/null)
    while read -r word number _ sha _; do
        path=$(grep -m1 "^$sha " <<<"$known" | cut -c67-)
        if [ -n "$path" ]; then
            printf 'pass %s -> %s\n' "$number" "$path"
        elif [ "$sha" = '?' ]; then
            printf 'pass %s -> the pipeline was not seen being created\n' "$number"
        else
            printf 'pass %s -> not in the known program folders (dumped at %s/programs/%s.dxil)\n' "$number" "$dir" "$sha"
        fi
    done < <(tr -d '\r' <"$output" | grep -a -E '^pass [0-9]+ = ')
}

run_profile() {
    local dir=$out/profile name=XmxXefgProfile exe=$tests_build/xmx/XmxXefgProfile.exe rc argument arguments=() reshade=0
    for argument in "$@"; do
        if [ "$argument" = --reshade ]; then reshade=1; else arguments+=("$argument"); fi
    done
    if [ ! -f "$exe" ]; then
        printf 'error: %s is not built; tools/linux/test.sh builds tests/xmx\n' "$exe" >&2
        return 1
    fi
    if [ "$reshade" = 1 ]; then
        dir=$out/profile-reshade
        name=XmxXefgProfileReShade
        mkdir -p "$dir"
        stage_profile_reshade "$dir" "$exe" || return 1
        exe=$dir/bin/XmxXefgProfile.exe
        arguments+=(--expect-wrapper)
        profile_reshade_header
    fi
    mkdir -p "$dir"
    rm -rf "$dir/programs"
    arguments+=(--programs "$dir/programs")
    : >"$dir/results.txt"
    nvidia-smi --query-compute-apps=pid,process_name,used_memory --format=csv
    ( export MFG_TEST_GPU=1; run_route_test "$dir" "$name" "$exe" "$xess/bin" "$build/MFGUnlocker.dll" "${arguments[@]}" ) || return $?
    cat "$dir/tests/$name/output.txt"
    name_profile_programs "$dir/tests/$name/output.txt" "$dir"
    rc=$(test_exit_code "$dir/tests/$name/output.txt")
    [ "$rc" = 0 ]
}

run_route() {
    local mode=$1 dir=$out/$1 status
    if [ "$mode" = intel-programs ]; then dump_intel_programs; return $?; fi
    ensure_inputs || return 1
    ensure_intel_programs || return 1
    stage_route "$mode" "$dir" || return 1
    build_route "$dir" || return 1
    check_reshade_copies "$mode" || return 1
    : >"$dir/results.txt"
    printf 'mode %s: fatbin %s\n' "$mode" "$(sha256sum "$dir/inputs/xefg_cuda_layers.fatbin" | cut -d' ' -f1)"
    run_route_tests "$dir"
    status=$?
    [ "$status" = 3 ] || run_xmx_contexts_test "$dir" || status=$?
    printf '=== %s: results ===\n' "$mode"
    cat "$dir/results.txt"
    return $status
}

mode=${1:-plain}
case $mode in
    build|all|route|intel-programs|profile|game-closed) ;;
    *) printf '%s\n' "${modes[@]}" | grep -qx "$mode" || usage ;;
esac

if [ "$mode" = game-closed ]; then
    game_is_open && exit 3
    exit 0
fi

if [ "$mode" = profile ]; then
    shift
    run_profile "$@"
    exit $?
fi

if printf '%s\n' "${route_modes[@]}" | grep -qx "$mode"; then
    run_route "$mode"
    exit $?
fi

build_cubin && build_exe || exit 1
[ "$mode" = build ] && exit 0

selected=("$mode")
[ "$mode" = all ] && selected=("${modes[@]}")
status=0
for run in "${selected[@]}"; do
    mode=$run
    run_mode "$run" || status=$?
done
exit $status
