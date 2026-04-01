# Development Workflow

This document captures the working agreement for developing BluButtonBridge with coding agents.

## 1. Default development cycle

For normal feature or fix work:

1. implement the change cleanly
2. run `idf.py -p /dev/cu.usbserial-0001 build flash`
3. let the user test on the board
4. only after successful validation proceed with commit / PR / merge

Behavior changes include:

- firmware logic
- boot/init flow
- OTA behavior
- web UI behavior
- REST API behavior
- WiFi / MQTT / BLE interactions
- status LEDs or runtime state handling

## Environment assumptions

Default local assumptions for this repo:

- ESP-IDF is installed locally
- the IDF environment can be loaded with:
  - `source ~/esp/esp-idf/export.sh`
- the default board flash port is:
  - `/dev/cu.usbserial-0001`
- GitHub CLI may be used for issue / PR / release work
- on-device behavioral validation is normally performed by the user after flash

If one of these assumptions is false, follow the fallback rules in [docs/VALIDATION.md](VALIDATION.md).

## 2. Multi-agent review flow

Non-trivial work must go through three independent reviews:

- `reviewer`
  - looks for bugs, regressions, hidden risks, missing test coverage
- `architect`
  - checks boundaries, ownership, layering, and whether the solution fits the project structure
- `simplifier`
  - looks for duplication, unnecessary branches, redundant state, and patch-on-patch complexity

### Completion rule

Review is not complete until:

- all three agents have returned a clear, usable result
- all actionable findings are fixed
- and there are no remaining obvious items to clean up

If one or more agents still find issues, keep iterating and rerun the reviews.
Do not stop at “good enough” if the agents are still pointing at real work to do.

### Tool instability rule

If sub-agent tooling is unstable or results are incomplete:

- say that explicitly
- do not claim that review is complete
- use build + hardware validation as the next discriminator only after being honest about the review gap

### What counts as non-trivial work

Treat work as non-trivial if it affects any of these:

- module boundaries or ownership
- boot or init flow
- OTA behavior
- runtime state or reconnect behavior
- REST API contracts
- persisted config schema
- web UI behavior beyond cosmetic-only text/style tweaks

## 3. Git and branch conventions

### Branch naming

Use repository-style branch names when possible:

- `feat/...`
- `fix/...`
- `refactor/...`
- `hardening/...`

Temporary Codex-generated branch names are acceptable, but stable work should converge to readable repo-style names when practical.

### PR policy

Use `branch + PR` for:

- refactors
- new components/modules
- changes in system behavior
- anything that materially affects architecture or runtime flow

Direct commits to `main` are acceptable only when explicitly aligned with the user for small, contained work.

### Branch retention

- Do not delete branches after merge unless explicitly asked.

### GitHub review bot

- Always ask the user before tagging `@codex review`.

## 4. Release and tag flow

When preparing a release:

1. ensure the target branch is merged to `main`
2. ensure the release commit is final
3. create the local git tag
4. run `idf.py reconfigure build`
5. verify the embedded firmware version matches the new tag
6. push commit and tag
7. create the GitHub release
8. upload both binaries:
   - `BluButtonBridge.bin`
   - `BluButtonBridge-full.bin`
9. record SHA-256 checksums for both binaries
10. write GitHub release notes using the standard repo format:
   - `## Changes`
   - 2-5 concrete bullets
   - optional `## Notes` only when needed
   - do not repeat artifact lists or checksum blocks in the body unless the user explicitly asks for them there

Why the tag comes before the build:

- this project derives firmware version from `git describe --tags --always --dirty`
- if you build before creating the new tag, the produced firmware may still embed the previous version context
- therefore the final release build must happen after the local tag exists

### Artifact notes

- `BluButtonBridge.bin` is the OTA binary
- `BluButtonBridge-full.bin` is the full flash image from `0x0`
- `dist/` is local generated output and should remain untracked unless explicitly requested otherwise

### Release note style

Preferred default:

```md
## Changes
- ...
- ...
- ...
```

Use `## Notes` only for short exceptional context such as:

- asset caveats
- migration steps
- known limits
- deprecation warnings

## 5. UI / UX guardrails

- Avoid visible load flicker when theme or active view can be resolved before first paint.
- If useful UI state should survive refresh or support deep links, prefer URL-based persistence when simple.
- Real-time console or streaming features must not monopolize the HTTP server task.

## 6. Testing expectations by area

### OTA changes

Test all of the following when OTA logic changes:

- GitHub OTA success path
- GitHub OTA failure path
- recovery to normal boot after failure
- manual upload OTA still works

### Web UI changes

Test at least:

- dark/light theme behavior
- refresh behavior
- mobile layout sanity
- any new persistence behavior such as URL hashes or retained state

### Runtime / connectivity changes

When touching WiFi/MQTT/BLE/system state flows, verify:

- normal boot
- reconnect behavior
- status reporting
- LED behavior if affected

## 7. Communication expectations

- Be explicit about whether a fix is a tactical patch or a robust design change.
- If the architecture starts drifting, pause and realign before piling on more fixes.
- Do not claim success without distinguishing:
  - static review confidence
  - build success
  - hardware validation
