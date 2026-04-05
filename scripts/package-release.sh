#!/usr/bin/env bash
set -euo pipefail

root_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
ROOT_DIR="$root_dir"
source "$root_dir/scripts/boards-lib.sh"

RELEASE_OWNER="robertoamd90"
RELEASE_REPO="blu-button-bridge"
OTA_MANIFEST_NAME="ota-manifest.json"
MANIFEST_MODE="${1:-}"

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
  dist/ota-manifest.json  (only when packaging all boards for a tagged release)
EOF
}

if [[ $# -ne 1 ]]; then
    usage
    exit 1
fi

dist_dir="$root_dir/dist"
release_tag=""
release_html_url=""
release_download_base=""
manifest_entries=()

if [[ "$MANIFEST_MODE" == "all" ]]; then
    release_tag="$(git -C "$root_dir" describe --tags --exact-match 2>/dev/null || true)"
    if [[ -z "$release_tag" ]]; then
        echo "Packaging all boards for release requires an exact git tag checkout." >&2
        exit 1
    fi
    release_html_url="https://github.com/${RELEASE_OWNER}/${RELEASE_REPO}/releases/tag/${release_tag}"
    release_download_base="https://github.com/${RELEASE_OWNER}/${RELEASE_REPO}/releases/download/${release_tag}"
fi

write_ota_manifest() {
    local manifest_out="$dist_dir/$OTA_MANIFEST_NAME"
    if [[ ${#manifest_entries[@]} -eq 0 ]]; then
        echo "No manifest entries available." >&2
        exit 1
    fi
    jq -s \
        --arg tag "$release_tag" \
        --arg html_url "$release_html_url" \
        '{
            schema_version: 1,
            tag: $tag,
            html_url: $html_url,
            boards: (map(to_entries[]) | from_entries)
        }' <(printf '%s\n' "${manifest_entries[@]}") > "$manifest_out"
    shasum -a 256 "$manifest_out"
}

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

    if [[ "$MANIFEST_MODE" == "all" ]]; then
        local ota_sha=""
        local ota_size=""
        ota_sha="$(shasum -a 256 "$ota_out" | awk '{print $1}')"
        ota_size="$(wc -c < "$ota_out" | tr -d '[:space:]')"
        manifest_entries+=("$(jq -cn \
            --arg board_id "$board_id" \
            --arg asset_name "$ota_name" \
            --arg asset_sha256 "sha256:${ota_sha}" \
            --arg browser_download_url "${release_download_base}/${ota_name}" \
            --argjson asset_size "$ota_size" \
            '{
                ($board_id): {
                    asset_name: $asset_name,
                    asset_size: $asset_size,
                    asset_sha256: $asset_sha256,
                    browser_download_url: $browser_download_url
                }
            }')")
    fi
    shasum -a 256 "$ota_out" "$full_out"
}

case "$1" in
    all)
        while IFS= read -r board_id; do
            package_board "$board_id"
        done < <(boards_list_ids)
        write_ota_manifest
        ;;
    *)
        package_board "$1"
        ;;
esac
