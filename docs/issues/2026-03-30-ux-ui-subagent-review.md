# UX/UI Subagent Review Log

## Pass 1
Findings:
1. Several inline text/border colors were hardcoded for light mode only; in dark mode this reduced contrast.
2. BLE device list warning badge for decrypt errors used a fixed orange tone inconsistent with dark tokens.

Actions taken:
- Replaced hardcoded inline colors with theme tokens (`--text-muted`, `--text-soft`, `--border`, `--warning-*`, `--error-*`).
- Updated decrypt warning badge color to `--error-text-soft`.

## Pass 2
Result: No blocking UX/UI issues found for this feature scope.

Notes:
- Theme switch is immediate.
- System mode follows OS preference and reacts to runtime changes.
- Theme selector is discoverable in System settings.
