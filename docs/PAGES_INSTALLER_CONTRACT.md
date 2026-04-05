# Pages Installer Contract

This document captures the contract between the GitHub release assets, the GitHub Pages workflow, and the browser installer site.

## Scope

Relevant files:

- `site/index.html`
- `site/app.js`
- `config/boards.json`
- `site/styles.css`
- `.github/workflows/pages.yml`
- `README.md`

This contract is about the public installer site served from GitHub Pages.

## Board catalog source of truth

Board identity for the installer and release asset naming is centralized in:

- `config/boards.json`

That catalog drives:

- installer board choices and chip-family metadata in `site/app.js`
- release asset download and mirrored metadata generation in `.github/workflows/pages.yml`
- build/profile aliases in local helper scripts
- generated OTA release-profile data consumed by the firmware

The Pages workflow publishes that shared catalog into the site root as `./boards.json` so the browser installer can load it without depending on repository layout.

## Published URL

Expected installer URL:

- `https://robertoamd90.github.io/blu-button-bridge/`

The README should link to that URL directly.

## Primary asset contract

The installer depends on the full flash image, not the OTA image.

Expected release assets:

- `BluButtonBridge-esp32-devkit-v1-full.bin`
- `BluButtonBridge-esp32c3-supermini-full.bin`

This matters because:

- the Pages workflow mirrors that file into the published site payload
- the browser installer manifest points at the mirrored same-origin copy
- if the latest release is missing that asset, browser install is blocked

## Pages workflow contract

Current workflow behavior in `.github/workflows/pages.yml`:

- triggers on GitHub `release.published`
- checks out the repository
- copies `site/` into `_site/`
- reads `config/boards.json`
- downloads every board-specific full image declared there from the just-published release into `_site/firmware/`
- copies that shared catalog to `_site/boards.json`
- uploads `_site/` as the Pages artifact
- deploys that artifact to GitHub Pages

Expected published payload includes:

- site HTML/CSS/JS from `site/`
- mirrored firmware at:
  - `./firmware/BluButtonBridge-esp32-devkit-v1-full.bin`
  - `./firmware/BluButtonBridge-esp32c3-supermini-full.bin`
- mirror metadata at `./firmware/metadata.json`

## Browser-side manifest contract

Current `site/app.js` behavior:

- loads board definitions from `./boards.json`
- falls back to `../config/boards.json` for local source-tree previews
- builds the `esp-web-tools` manifest dynamically in the browser
- switches between same-origin board-specific firmware URLs
- reads mirrored asset metadata from `./firmware/metadata.json`
- uses the latest public GitHub release metadata to populate:
  - version
  - publish date
  - size
  - release link
- compares the latest release asset digest with the mirrored asset digest before allowing browser install

The actual install button flashes the same-origin mirrored full image for the selected board, not a GitHub asset URL directly.

## Release metadata and fallback behavior

Current metadata source:

- `https://api.github.com/repos/robertoamd90/blu-button-bridge/releases/latest`

Current success behavior:

- release metadata loads
- the latest release contains the selected board's full image asset
- the mirrored metadata file contains the selected board entry
- the mirrored asset digest matches the latest release asset digest for the selected board
- the install button remains visible
- the manifest version is updated to the release tag
- the UI shows release metadata

Current fallback behavior when metadata fetch fails or returns non-OK:

- the installer falls back to the mirrored Pages metadata when available
- the install button still points at the mirrored Pages asset
- the UI warns that live metadata is unavailable

Current failure behavior when metadata loads but the latest release is missing the selected board asset:

- the install button is hidden
- the UI shows an error state
- the mirrored asset link remains available

Current failure behavior when the mirrored asset cannot be confirmed to match the latest release:

- the install button is hidden
- the UI shows a warning state
- the direct download link points to the latest release asset instead of the mirrored Pages copy

## Browser UX expectations

Current page copy assumes:

- Chrome or Edge on a desktop-class browser
- HTTPS
- Web Serial availability

Current board selector expectation:

- ESP32 DevKit V1
- ESP32-C3 SuperMini

If the supported board list or full image layout changes, update both the installer page and this contract.

## Change guardrails

Before changing the installer flow, verify:

- whether `config/boards.json` still matches the supported boards and release asset names
- whether the required release asset name is changing
- whether the workflow still mirrors the full image into `./firmware/`
- whether the workflow also writes matching metadata into `./firmware/metadata.json`
- whether the browser manifest still points at a same-origin copy
- whether the browser still compares release digest vs mirrored digest before enabling install
- whether README links and user-facing instructions still match the deployed site

If you change:

- the release asset name
- the mirrored firmware path
- the published Pages URL
- the fallback behavior
- the board or flash assumptions shown on the installer page

update both:

- this document
- `README.md`
- `site/index.html`
- `site/app.js`
- `config/boards.json`
- `.github/workflows/pages.yml`

## Validation hints

When touching the Pages installer flow, validate at least:

- the README installer link is clickable and points to the expected public URL
- `site/` still renders with no broken local asset references
- the install button manifest points at the selected board's mirrored firmware path
- the happy path works when the latest release includes the required asset
- the happy path confirms digest match between GitHub metadata and mirrored metadata
- the metadata-fallback path still leaves the install button usable
- the missing-asset path hides the install button and shows an explicit error
- the digest-mismatch path hides the install button and points manual download at the latest release asset
