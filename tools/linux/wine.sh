#!/usr/bin/env bash
export WINEPREFIX=${MFG_WINEPREFIX:-$HOME/.cache/mfg-unlocker/wineprefix}
export WINEARCH=win64
export WINEDEBUG=${WINEDEBUG:--all}
export WINEDLLOVERRIDES=${WINEDLLOVERRIDES:-mscoree,mshtml=}
exec wine "$@"
