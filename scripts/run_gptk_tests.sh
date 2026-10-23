#!/usr/bin/env bash
# Run the real-GPU GPTk tests in build/gptk/ under Wine + GPTk (D3DMetal), using the
# environment GPTk-based launchers (e.g. MacHoYo) use to launch games.
#
# Settings (environment or scripts/local.env, see scripts/local.env.example): WINE_BIN, WINEPREFIX
# (default: MacHoYo's runtime), TEST_TIMEOUT (seconds per test), FG_PACING / FG_CAPTURE
# (metalstream.ini FGPacing / FGCapture)
# Usage: scripts/run_gptk_tests.sh [test_name[:variant] ...]   (default: all of them)
#   test_gptk_framegen_swapchain:sync0     without vsync, game uncapped (present thread must not bunch presents)
#   test_gptk_framegen_swapchain:sync0cap  without vsync, game capped at half the refresh rate (even pacing)
#   test_gptk_framegen_lifecycle           resize / recreate / second swapchain / slShutdown while the present thread runs
set -u

source "$(dirname "${BASH_SOURCE[0]}")/common.sh"
RUNTIME="${HOME}/Library/Application Support/Starward/runtime"
WINE_BIN="${WINE_BIN:-${RUNTIME}/wine/bin/wine}"
export WINEPREFIX="${WINEPREFIX:-${RUNTIME}/wineprefix_nap}"
TEST_TIMEOUT="${TEST_TIMEOUT:-120}"
GPTK_DIR="${PROJECT_DIR}/build/gptk"

export WINEMSYNC=1
export WINEDEBUG=-all
export D3DM_ENABLE_METALFX=1
export D3DM_NVNGX_PATH='C:\windows\system32\nvngx.dll'
export D3DM_VENDOR_ID=0x10de
export D3DM_DEVICE_ID=0x2684
export WINEDLLOVERRIDES="d3d10,d3d11,d3d12,d3d12core,dxgi,nvngx,nvapi64=n,b"

if [ ! -x "${WINE_BIN}" ]; then
    echo "Wine not found at ${WINE_BIN} (set WINE_BIN / WINEPREFIX, see scripts/local.env.example)" >&2
    exit 2
fi

ninja -C "${PROJECT_DIR}/build" test_gptk_fg_probe test_gptk_framegen_swapchain test_gptk_framegen_lifecycle >/dev/null || exit 2
# The POST_BUILD copy only runs when the test relinks; always test the current interposer
cp -f "${PROJECT_DIR}/build/sl.interposer.dll" "${GPTK_DIR}/" || exit 2

write_ini() {
    # Verbose MetalStream logging for the swapchain test
    printf '[MetalStream]\nEnableLogging=1\nDebug=0\nFGPacing=%s\nFGCapture=%s\nFGMaxGameFps=%s\n' \
        "${FG_PACING:-1}" "${FG_CAPTURE:-2}" "$1" > "${GPTK_DIR}/metalstream.ini"
}

if [ $# -gt 0 ]; then
    TESTS=("$@")
else
    TESTS=(test_gptk_fg_probe test_gptk_framegen_swapchain test_gptk_framegen_swapchain:sync0 test_gptk_framegen_swapchain:sync0cap
           test_gptk_framegen_lifecycle)
fi

failed=0
cd "${GPTK_DIR}" || exit 2
for entry in "${TESTS[@]}"; do
    t="${entry%%:*}"
    out="${entry//:/_}"
    unset METALSTREAM_TEST_SYNC METALSTREAM_TEST_EXPECT_EVEN
    case "${entry}" in
        *:sync0) export METALSTREAM_TEST_SYNC=0; write_ini 0 ;;
        *:sync0cap) export METALSTREAM_TEST_SYNC=0 METALSTREAM_TEST_EXPECT_EVEN=1; write_ini -1 ;;
        test_gptk_framegen_lifecycle) write_ini -1 ;; # capped, so every game frame shows a generated frame
        *) write_ini 0 ;;
    esac
    echo "==> ${entry}"
    rm -f metalstream.log
    rm -rf metalstream_capture
    "${WINE_BIN}" "${t}.exe" > "${out}.out" 2>&1 &
    pid=$!
    elapsed=0
    while kill -0 "${pid}" 2>/dev/null && [ "${elapsed}" -lt "${TEST_TIMEOUT}" ]; do
        sleep 1
        elapsed=$((elapsed + 1))
    done
    if kill -0 "${pid}" 2>/dev/null; then
        kill "${pid}" 2>/dev/null
        echo "    TIMEOUT after ${TEST_TIMEOUT}s"
        failed=1
    else
        wait "${pid}"
        rc=$?
        [ "${rc}" -ne 0 ] && failed=1
    fi
    grep -E '^\[(PASS|FAIL|INFO)\]|^RESULT' "${out}.out" | sed 's/^/    /'
    if [ -f metalstream.log ]; then
        cp metalstream.log "${out}.metalstream.log"
        grep -E 'framegen' metalstream.log | sed 's/^/    log: /' | tail -20
    fi
done
exit "${failed}"
