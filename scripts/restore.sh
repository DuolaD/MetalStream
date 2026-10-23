#!/usr/bin/env bash
# Put back the original Streamline DLLs that deploy.sh saved as *.orig.
# Usage: scripts/restore.sh [game_dir]   (default: METALSTREAM_GAME_DIR, see scripts/local.env.example)
set -e

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
resolve_game_dir "${1:-}"

echo "==> Restoring original Streamline DLLs in ${GAME_DIR}..."
if [ -f "${GAME_DIR}/sl.interposer.dll.orig" ]; then
    cp -f "${GAME_DIR}/sl.interposer.dll.orig" "${GAME_DIR}/sl.interposer.dll"
    echo "Restored sl.interposer.dll"
fi

if [ -f "${GAME_DIR}/sl.dlss.dll.orig" ]; then
    cp -f "${GAME_DIR}/sl.dlss.dll.orig" "${GAME_DIR}/sl.dlss.dll"
    echo "Restored sl.dlss.dll"
fi

echo "==> Original DLLs restored successfully."
