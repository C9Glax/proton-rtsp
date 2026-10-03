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
  "$proton/proton" run "$here/kmtest.exe" "${2:-300}" "${3:-20}" "${4:-1920}" 2>&1 | grep -v '^\(fsync\|esync\|wineserver\)' || true
done
