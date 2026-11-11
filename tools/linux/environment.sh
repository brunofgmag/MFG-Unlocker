here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
repository=$(cd "$here/../.." && pwd)
native=$repository/source/native
export PYTHONDONTWRITEBYTECODE=1

cache=${MFG_CACHE:-$HOME/.cache/mfg-unlocker}
streamline=${MFG_STREAMLINE_ROOT:-$cache/sdk/streamline-sdk-v2.14.1}
xess=${MFG_XESS_SDK_ROOT:-$cache/sdk/xess-sdk-3.0.2}
imgui=${MFG_IMGUI_ROOT:-$HOME/Projetos/renodx/external/reshade/deps/imgui}
vulkan12_git=${MFG_VULKAN12_HEADERS_GIT:-$HOME/Projetos/vkd3d-proton-fenix-cachyos-11.0-20261005-slr-20261020-093638/khronos/Vulkan-Headers}
vulkan14_include=${MFG_VULKAN14_INCLUDE:-/usr/include}
release_zip=${MFG_RELEASE_ZIP:-$HOME/Jogos/Mods/cyberpunk2077/mfg-unlocker-dashdogy-1.7.0/MFGUnlocker-v1.7.0.zip}
inputs=${MFG_INPUTS_DIR:-$cache/inputs/v1.7.0}
build=${MFG_BUILD_DIR:-$repository/build/linux}
xwin=${MFG_XWIN:-$HOME/.xwin}
dxc=${MFG_DXC:-$HOME/.local/dxc/bin/dxc}
export MFG_XWIN=$xwin MFG_DXC=$dxc

declare -A root

first_line() {
    "$@" 2>&1 | head -n 1 || true
}

print_versions() {
    local sdk_version
    sdk_version=$(cd "$xwin/sdk/include" 2>/dev/null && ls -d [0-9]* 2>/dev/null | head -n 1 || true)
    printf 'clang-cl  %s\n' "$(first_line clang-cl --version)"
    printf 'lld-link  %s\n' "$(first_line lld-link --version)"
    printf 'cmake     %s\n' "$(first_line cmake --version)"
    printf 'ninja     %s\n' "$(first_line ninja --version)"
    printf 'xwin crt  MSVC STL %s\n' "$(grep -h -E '#define _MSVC_STL_(VERSION|UPDATE)' "$xwin/crt/include/yvals_core.h" 2>/dev/null | awk '{print $3}' | paste -sd' ' || true)"
    printf 'xwin sdk  %s\n' "${sdk_version:-unknown}"
    printf 'dxc       %s\n' "$(first_line "$dxc" --version)"
    printf 'python    %s\n' "$(first_line python3 --version)"
}

resolve_sdk_roots() {
    local resolved key value
    resolved=$(python3 -I "$here/check_sdk.py" \
        --build-script "$native/build.ps1" --native "$native" \
        --streamline "$streamline" --xess "$xess" --imgui "$imgui" \
        --vulkan12-git "$vulkan12_git" --vulkan14-include "$vulkan14_include" \
        --copies "$cache/sdk/crlf")
    while IFS== read -r key value; do
        root[$key]=$value
    done <<<"$resolved"
}

ensure_inputs() {
    if [ ! -d "$inputs" ]; then
        python3 -I "$here/extract_inputs.py" extract --zip "$release_zip" --inputs "$inputs"
    fi
    python3 -I "$here/extract_inputs.py" verify --inputs "$inputs"
}
