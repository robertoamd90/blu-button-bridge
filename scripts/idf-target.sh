#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$root_dir"
source "$root_dir/scripts/boards-lib.sh"

usage() {
    local board_tokens=""
    if command -v jq >/dev/null 2>&1; then
        board_tokens="$(boards_list_tokens | sed 's/^/  /')"
    else
        board_tokens="  jq is required to list supported board tokens"
    fi

    cat <<'EOF'
Usage: scripts/idf-target.sh <target> <action> [extra idf.py args...]

Targets:
EOF
    printf '%s\n' "$board_tokens"
    cat <<'EOF'

Actions:
  build
  flash
  monitor
  size
  menuconfig
  reconfigure
  clean
  fullclean

Examples:
  scripts/idf-target.sh esp32 build
  scripts/idf-target.sh esp32 flash
  scripts/idf-target.sh esp32c3 build
  ESPPORT=/dev/cu.usbmodem123 scripts/idf-target.sh esp32c3 flash
EOF
}

if [[ $# -lt 2 ]]; then
    usage
    exit 1
fi

board="$1"
action="$2"
shift 2

default_port="${ESPPORT:-/dev/cu.usbserial-0001}"

if ! board_json="$(boards_resolve_json "$board")"; then
    echo "Unknown target: $board" >&2
    usage
    exit 1
fi

idf_target="$(jq -r '.target' <<<"$board_json")"
sdkconfig_path="$root_dir/$(jq -r '.sdkconfig' <<<"$board_json")"
build_dir="$root_dir/$(jq -r '.build_dir' <<<"$board_json")"

idf_args=(
    -B "$build_dir"
    "-DSDKCONFIG=$sdkconfig_path"
    "-DIDF_TARGET=$idf_target"
)

case "$action" in
    build)
        exec idf.py "${idf_args[@]}" build "$@"
        ;;
    flash)
        exec idf.py "${idf_args[@]}" -p "$default_port" build flash "$@"
        ;;
    monitor)
        exec idf.py "${idf_args[@]}" -p "$default_port" monitor "$@"
        ;;
    size)
        exec idf.py "${idf_args[@]}" size "$@"
        ;;
    menuconfig)
        exec idf.py "${idf_args[@]}" menuconfig "$@"
        ;;
    reconfigure)
        exec idf.py "${idf_args[@]}" reconfigure "$@"
        ;;
    clean)
        exec idf.py "${idf_args[@]}" clean "$@"
        ;;
    fullclean)
        exec idf.py "${idf_args[@]}" fullclean "$@"
        ;;
    *)
        echo "Unknown action: $action" >&2
        usage
        exit 1
        ;;
esac
