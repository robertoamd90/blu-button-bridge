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
Usage: scripts/package-release.sh <board|all>

Boards:
EOF
    printf '%s\n' "$board_tokens"
    cat <<'EOF'
  all

Outputs:
  dist/BluButtonBridge-<board>.bin
  dist/BluButtonBridge-<board>-full.bin
EOF
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

dist_dir="$root_dir/dist"

package_board() {
    local board="$1"
    local board_json=""
    local board_id=""
    local build_dir=""
    local chip=""
    local ota_name=""
    local full_name=""

    if ! board_json="$(boards_resolve_json "$board")"; then
        echo "Unknown board: $board" >&2
        usage
        exit 1
    fi

    board_id="$(jq -r '.id' <<<"$board_json")"
    build_dir="$root_dir/$(jq -r '.build_dir' <<<"$board_json")"
    chip="$(jq -r '.chip' <<<"$board_json")"
    ota_name="$(jq -r '.ota_asset_name' <<<"$board_json")"
    full_name="$(jq -r '.full_asset_name' <<<"$board_json")"

    if [[ ! -f "$build_dir/BluButtonBridge.bin" ]]; then
        echo "Missing OTA binary in $build_dir. Run the build first." >&2
        exit 1
    fi
    if [[ ! -f "$build_dir/flash_args" ]]; then
        echo "Missing flash_args in $build_dir. Run the build first." >&2
        exit 1
    fi

    mkdir -p "$dist_dir"

    local ota_out="$dist_dir/$ota_name"
    local full_out="$dist_dir/$full_name"

    cp "$build_dir/BluButtonBridge.bin" "$ota_out"

    local merge_args=()
    read -r -a merge_args <<< "$(head -n 1 "$build_dir/flash_args")"
    while read -r addr file; do
        [[ -n "${addr:-}" ]] || continue
        merge_args+=("$addr" "$file")
    done < <(tail -n +2 "$build_dir/flash_args")

    (
        cd "$build_dir"
        esptool --chip "$chip" merge-bin -o "$full_out" "${merge_args[@]}"
    )

    shasum -a 256 "$ota_out" "$full_out"
}

case "$1" in
    all)
        while IFS= read -r board_id; do
            package_board "$board_id"
        done < <(boards_list_ids)
        ;;
    *)
        package_board "$1"
        ;;
esac
