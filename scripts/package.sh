#!/usr/bin/env bash
# Package a build for release: MetalStream-<version>.zip (stripped DLLs + sample ini),
# MetalStream-<version>-debug-symbols.zip and SHA256SUMS.
# Usage: scripts/package.sh <version> [build_dir] [out_dir]   (defaults: build, dist)
set -euo pipefail

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"

VERSION="${1:?Usage: scripts/package.sh <version> [build_dir] [out_dir]}"
BUILD_DIR="$(cd "${2:-${PROJECT_DIR}/build}" && pwd)"
OUT_DIR="${3:-${PROJECT_DIR}/dist}"
ZIP="MetalStream-${VERSION//\//-}.zip" # a tag like release/1.0 must not become a directory
SYMBOLS_ZIP="${ZIP%.zip}-debug-symbols.zip"
DLLS=(sl.interposer.dll sl.dlss.dll)
MINGW_PREFIX="${MINGW_PREFIX:-x86_64-w64-mingw32}" # same override as cmake/x86_64-w64-mingw32.cmake
OBJCOPY="${MINGW_PREFIX}-objcopy"
STRIP="${MINGW_PREFIX}-strip"
OBJDUMP="${MINGW_PREFIX}-objdump"

echo "==> Checking DLL imports (static runtime, Universal CRT only)..."
for dll in "${DLLS[@]}"; do
    bad="$("${OBJDUMP}" -p "${BUILD_DIR}/${dll}" | sed -n 's/^[[:space:]]*DLL Name: //p' \
        | grep -viE '^(kernel32|user32)\.dll$|^api-ms-win-crt-' || true)"
    if [ -n "${bad}" ]; then
        echo "${dll} imports DLLs a game folder won't have (wrong CRT or dynamic runtime?):" >&2
        echo "${bad}" >&2
        exit 1
    fi
done

rm -rf "${OUT_DIR}"
mkdir -p "${OUT_DIR}/stage" "${OUT_DIR}/symbols"
OUT_DIR="$(cd "${OUT_DIR}" && pwd)"

echo "==> Stripping DLLs (debug info goes to the symbols archive)..."
for dll in "${DLLS[@]}"; do
    cp "${BUILD_DIR}/${dll}" "${OUT_DIR}/stage/${dll}"
    "${OBJCOPY}" --only-keep-debug "${OUT_DIR}/stage/${dll}" "${OUT_DIR}/symbols/${dll}.debug"
    "${STRIP}" --strip-unneeded "${OUT_DIR}/stage/${dll}"
    (cd "${OUT_DIR}/symbols" && "${OBJCOPY}" --add-gnu-debuglink="${dll}.debug" "${OUT_DIR}/stage/${dll}")
done

cp "${PROJECT_DIR}/metalstream.ini" "${PROJECT_DIR}/LICENSE" "${OUT_DIR}/stage/"
cat > "${OUT_DIR}/stage/README.txt" <<EOF
MetalStream ${VERSION}

Copy sl.interposer.dll and sl.dlss.dll into the game folder that already contains them
(back up the game's originals first). metalstream.ini is optional.
Instructions: https://github.com/DuolaD/MetalStream#installing-a-release
EOF

echo "==> Writing archives to ${OUT_DIR}..."
(cd "${OUT_DIR}/stage" && zip -q -X "${OUT_DIR}/${ZIP}" ./*)
(cd "${OUT_DIR}/symbols" && zip -q -X "${OUT_DIR}/${SYMBOLS_ZIP}" ./*)
rm -rf "${OUT_DIR}/stage" "${OUT_DIR}/symbols"

if command -v sha256sum >/dev/null; then
    (cd "${OUT_DIR}" && sha256sum "${ZIP}" "${SYMBOLS_ZIP}" > SHA256SUMS)
else
    (cd "${OUT_DIR}" && shasum -a 256 "${ZIP}" "${SYMBOLS_ZIP}" > SHA256SUMS)
fi
cat "${OUT_DIR}/SHA256SUMS"
