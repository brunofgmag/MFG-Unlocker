#!/usr/bin/env bash
set -uo pipefail

if [ -n "${MFG_HEADLESS_SESSION:-}" ]; then
    run=$MFG_HEADLESS_SESSION
    unset MFG_HEADLESS_SESSION
    eval "set -- $(cat "$run.args")"
    "$@" <&5 >&3 2>&4
    printf '%s' "$?" >"$run.status"
    exit 0
fi

if [ "${MFG_HEADLESS:-1}" = 0 ] || [ -n "${MFG_HEADLESS_ACTIVE:-}" ]; then
    MFG_HEADLESS_ACTIVE=1 exec "$@"
fi

session=$(mktemp -d "${TMPDIR:-/tmp}/mfg-headless.XXXXXX") || exit 1
trap 'rm -rf "$session"' EXIT
printf '%q ' "$@" >"$session/run.args"
: >"$session/run.status"
MFG_HEADLESS_ACTIVE=1 MFG_HEADLESS_SESSION=$session/run \
    kwin_wayland --virtual --xwayland --no-lockscreen --socket "mfg-headless-$$" \
    --exit-with-session "$(realpath "${BASH_SOURCE[0]}")" 3>&1 4>&2 5<&0 >"$session/kwin.log" 2>&1 </dev/null
status=$(cat "$session/run.status")
if [ -z "$status" ]; then
    printf 'headless.sh: the command did not run in the virtual KWin session; its log follows\n' >&2
    tail -n 20 "$session/kwin.log" >&2
    exit 125
fi
exit "$status"
