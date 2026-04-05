#!/usr/bin/env bash

boards_catalog_path() {
    echo "${ROOT_DIR}/config/boards.json"
}

boards_require_jq() {
    if ! command -v jq >/dev/null 2>&1; then
        echo "jq is required to read $(boards_catalog_path)" >&2
        exit 1
    fi
}

boards_resolve_json() {
    local query="$1"
    boards_require_jq
    jq -ce --arg q "$query" '
        .boards[]
        | select((.id == $q) or ((.aliases // []) | index($q)))
    ' "$(boards_catalog_path)"
}

boards_list_ids() {
    boards_require_jq
    jq -r '.boards[].id' "$(boards_catalog_path)"
}

boards_list_tokens() {
    boards_require_jq
    jq -r '.boards[] | [.id] + (.aliases // []) | .[]' "$(boards_catalog_path)" | sort -u
}
