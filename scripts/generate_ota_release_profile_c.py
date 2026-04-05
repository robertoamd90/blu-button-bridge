#!/usr/bin/env python3
import json
import sys
from pathlib import Path


def c_string(value: str) -> str:
    escaped = value.replace("\\", "\\\\").replace('"', '\\"')
    return f'"{escaped}"'


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: generate_ota_release_profile_c.py <boards.json> <output.c>", file=sys.stderr)
        return 1

    boards_path = Path(sys.argv[1])
    output_path = Path(sys.argv[2])

    data = json.loads(boards_path.read_text())
    boards = data.get("boards", [])
    if not boards:
        print(f"{boards_path} does not contain any boards", file=sys.stderr)
        return 1

    lines = [
        '#include "sdkconfig.h"',
        '#include "ota_release_profile.h"',
        "",
    ]

    for idx, board in enumerate(boards):
        prefix = "#if" if idx == 0 else "#elif"
        lines.extend([
            f'{prefix} defined({board["kconfig_symbol"]})',
            f'static const char *s_board_id = {c_string(board["id"])};',
            f'static const char *s_display_name = {c_string(board["display_name"])};',
            f'static const char *s_ota_asset_name = {c_string(board["ota_asset_name"])};',
            f'static const char *s_full_asset_name = {c_string(board["full_asset_name"])};',
            "",
        ])

    lines.extend([
        "#else",
        '#error "Unsupported BluButtonBridge board profile for OTA release data"',
        "#endif",
        "",
        "const char *ota_release_profile_board_id(void)",
        "{",
        "    return s_board_id;",
        "}",
        "",
        "const char *ota_release_profile_display_name(void)",
        "{",
        "    return s_display_name;",
        "}",
        "",
        "const char *ota_release_profile_ota_asset_name(void)",
        "{",
        "    return s_ota_asset_name;",
        "}",
        "",
        "const char *ota_release_profile_full_asset_name(void)",
        "{",
        "    return s_full_asset_name;",
        "}",
        "",
    ])

    output_path.parent.mkdir(parents=True, exist_ok=True)
    output_path.write_text("\n".join(lines))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
