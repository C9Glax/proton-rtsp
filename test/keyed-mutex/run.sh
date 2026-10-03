#!/bin/bash
# Runs kmtest.exe with the default (fixed) behaviour and with DXVK_KEYED_MUTEX_BLOCKING=1 (old behaviour).
#
# Usage: ./run.sh [path to proton build dir] [frames] [load] [size]
#   proton dir default: ~/.steam/steam/compatibilitytools.d/<newest entry>
#   (the directory that contains the "proton" script)
set -e
here="$(cd "$(dirname "$0")" && pwd)"
steam="${STEAM_COMPAT_CLIENT_INSTALL_PATH:-$HOME/.steam/steam}"
proton="${1:-}"
if [ -z "$proton" ]; then
  proton="$(ls -dt "$steam"/compatibilitytools.d/*/ | head -n1)"
fi
proton="${proton%/}"
[ -x "$proton/proton" ] || { echo "No proton script in '$proton'"; exit 1; }

export STEAM_COMPAT_CLIENT_INSTALL_PATH="$steam"
export STEAM_COMPAT_DATA_PATH="${STEAM_COMPAT_DATA_PATH:-$HOME/.cache/kmtest-prefix}"
mkdir -p "$STEAM_COMPAT_DATA_PATH"

echo "Using $proton"
for mode in fixed blocking; do
  echo; echo "=== $mode ==="
  if [ "$mode" = blocking ]; then export DXVK_KEYED_MUTEX_BLOCKING=1; else unset DXVK_KEYED_MUTEX_BLOCKING; fi
  rm -f "$here/kmtest-$mode.log"
  mkdir -p "$here/logs"
  rc=0
  rm -f "$here"/logs/*.log; PROTON_LOG=1 PROTON_LOG_DIR="$here/logs" DXVK_LOG_PATH="$here/logs" DXVK_LOG_LEVEL=info "$proton/proton" run "$here/kmtest.exe" "${2:-300}" "${3:-20}" "${4:-1920}" >/dev/null 2>&1 || rc=$?
  if [ -s "$here/kmtest-$mode.log" ]; then
    cat "$here/kmtest-$mode.log"
  else
    echo "No output was written (exit code $rc): kmtest.exe did not start or crashed."
    echo "Check $here/logs/steam-*.log (search for 'err:' and 'kmtest')."
  fi
  echo "--- DXVK warnings/errors ($mode) ---"
  grep -hiE 'warn|err|keyed|shared' "$here"/logs/*d3d11.log "$here"/logs/*dxgi.log 2>/dev/null | head -n 20 || true
  cp -f "$here"/logs/*d3d11.log "$here/logs/last-$mode-d3d11.txt" 2>/dev/null || true
done
