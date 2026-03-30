# Feature Issue: Dark Theme + Theme Mode Selector

## Summary
Add full dark theme support to the Web UI and a user-facing selector in **System** settings to choose:
- **System mode** (follows operating system theme)
- **Light**
- **Dark**

## Goals
- Improve readability in low-light environments.
- Keep visual consistency across all UI tabs/cards/forms.
- Persist user preference locally in the browser.
- Make System mode dynamically react to OS theme changes.

## UX Requirements
- Theme selector appears in the **System** tab in a dedicated card.
- Selecting a mode should apply instantly.
- A short confirmation toast should appear when the user changes theme.
- In System mode, changing OS theme should update UI without reload.

## Technical Notes
- Use CSS custom properties for light/dark tokens.
- Drive active theme from `body[data-theme]`.
- Save user mode to `localStorage` key: `bbb-ui-theme`.
- Resolve effective theme at startup (with pre-hydration script to reduce flash).

## Acceptance Criteria
- UI is legible in both light and dark mode.
- Selector options are exactly: `System mode (follow OS)`, `Light`, `Dark`.
- Theme persists across page reloads.
- System mode follows OS preference and updates on OS changes.
