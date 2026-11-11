#!/usr/bin/env bash
set -euo pipefail
arguments=("$@")
last=$((${#arguments[@]} - 1))
source=${arguments[$last]}
staged=$(mktemp --suffix=.asm)
trap 'rm -f "$staged"' EXIT
sed -E 's/^[[:space:]]*option[[:space:]]+casemap:none[[:space:]]*$//I' "$source" > "$staged"
arguments[$last]=$staged
llvm-ml -m64 "${arguments[@]}"
