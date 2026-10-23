#!/usr/bin/env bash
# Build, strip and copy sl.interposer.dll / sl.dlss.dll into a game directory.
# Usage: scripts/deploy.sh [game_dir]   (default: METALSTREAM_GAME_DIR, see scripts/local.env.example)
set -e

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
resolve_game_dir "${1:-}"

echo "==> Building MetalStream..."
ninja -C "${PROJECT_DIR}/build"

echo "==> Stripping debug symbols (kept in build/*.dll.debug for crash symbolication)..."
for dll in sl.interposer.dll sl.dlss.dll; do
    x86_64-w64-mingw32-objcopy --only-keep-debug "${PROJECT_DIR}/build/${dll}" "${PROJECT_DIR}/build/${dll}.debug"
    x86_64-w64-mingw32-strip "${PROJECT_DIR}/build/${dll}"
done

echo "==> Ensuring backups in game directory..."
if [ -f "${GAME_DIR}/sl.interposer.dll" ] && [ ! -f "${GAME_DIR}/sl.interposer.dll.orig" ]; then
    cp "${GAME_DIR}/sl.interposer.dll" "${GAME_DIR}/sl.interposer.dll.orig"
fi
if [ -f "${GAME_DIR}/sl.dlss.dll" ] && [ ! -f "${GAME_DIR}/sl.dlss.dll.orig" ]; then
    cp "${GAME_DIR}/sl.dlss.dll" "${GAME_DIR}/sl.dlss.dll.orig"
fi

echo "==> Deploying MetalStream to ${GAME_DIR}..."
cp "${PROJECT_DIR}/build/sl.interposer.dll" "${GAME_DIR}/sl.interposer.dll"
cp "${PROJECT_DIR}/build/sl.dlss.dll" "${GAME_DIR}/sl.dlss.dll"

echo "==> Deployment complete!"
echo "Check metalstream.log in game folder when running the game."
