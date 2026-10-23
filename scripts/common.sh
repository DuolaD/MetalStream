# Sourced by the helper scripts: loads scripts/local.env without overriding variables already set
# in the environment.
SCRIPTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "${SCRIPTS_DIR}/.." && pwd)"

if [ -f "${SCRIPTS_DIR}/local.env" ]; then
    while IFS= read -r line || [ -n "${line}" ]; do
        case "${line}" in ''|'#'*) continue ;; esac
        name="${line%%=*}"
        [ -n "${!name+x}" ] && continue # the environment wins
        eval "export ${line}"
    done < "${SCRIPTS_DIR}/local.env"
fi

# Resolves the game directory from $1 or METALSTREAM_GAME_DIR, or exits with a usage message
resolve_game_dir() {
    GAME_DIR="${1:-${METALSTREAM_GAME_DIR:-}}"
    if [ -z "${GAME_DIR}" ]; then
        echo "Game directory not set: pass it as the first argument, or set METALSTREAM_GAME_DIR" >&2
        echo "(environment or scripts/local.env, see scripts/local.env.example)" >&2
        exit 2
    fi
    if [ ! -d "${GAME_DIR}" ]; then
        echo "Game directory not found: ${GAME_DIR}" >&2
        exit 2
    fi
}
