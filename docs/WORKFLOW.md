# Development Workflow

This document captures the working agreement for developing BluButtonBridge with coding agents.

## 1. Default development cycle

For normal feature or fix work:

1. implement the change cleanly
2. run a target-aware build+flash for the active board profile
3. let the user test on the board
4. only after successful validation and any user-requested review proceed with commit / PR / merge

Examples:

- `source ~/esp/esp-idf-v6.0/export.sh && scripts/idf-target.sh esp32-devkit-v1 flash`
- `source ~/esp/esp-idf-v6.0/export.sh && ESPPORT=/dev/cu.usbmodem3101 scripts/idf-target.sh esp32c3-supermini flash`

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
  - `source ~/esp/esp-idf-v6.0/export.sh`
- the default board flash port is:
  - `/dev/cu.usbserial-0001`
- GitHub CLI may be used for issue / PR / release work
- on-device behavioral validation is normally performed by the user after flash

If one of these assumptions is false, follow the fallback rules in [docs/VALIDATION.md](VALIDATION.md).

## 2. User-directed review flow

Review-phase agents are opt-in for this repository.
Do not invoke review agents unless the user explicitly asks for them in the
current task.

Default rule:

- do not automatically start review agents after implementation
- do not automatically enter a `fix -> review -> fix -> review` loop
- if review agents were not requested, say that explicitly in the handoff

When the user explicitly asks for a review round on non-trivial work, the
default review set is three independent reviews:

- `reviewer`
  - looks for bugs, regressions, hidden risks, and missing validation
- `architect`
  - checks boundaries, ownership, layering, and fit with the intended project structure
- `simplifier`
  - looks for duplication, unnecessary branches, redundant state, and patch-on-patch complexity

When the user explicitly asks for a review round and the change significantly affects repository documentation, onboarding flow, workflow guidance, or agent instructions, also run:

- `librarian`
  - reviews documentation clarity, source-of-truth hierarchy, onboarding speed, task discoverability, actionability, and AI-agent friendliness

This section applies to review-phase agents only.
It does not impose the same response format on generic agents doing implementation or exploration work.

Stable reviewer-role definitions live in:

- `.codex/agents/reviewer.toml`
- `.codex/agents/architect.toml`
- `.codex/agents/simplifier.toml`
- `.codex/agents/librarian.toml`

Treat those files as the source of truth for reviewer identity, mandate,
forbidden actions, and output format.
When the host supports project-scoped custom subagent discovery, the review
runner should load the named subagents from those files as entry points.
The invocation prompt should add only the current review scope, objective, and
any truly local emphasis.

### Review-agent output contract

Each reviewer's exact output contract is defined in the matching
`.codex/agents/*.toml` file.
`docs/WORKFLOW.md` should be treated only as invocation and runner guidance.

### Review invocation discipline

When invoking review-phase agents, spawn the named custom subagent for the role
from `.codex/agents/` and prompt it as a single-purpose reviewer, not as a
coordinator.

The named custom subagent already carries the reviewer contract.
Do not restate the full prohibition set in the task prompt unless tooling
limitations prevent the subagent definition from being loaded.

Expected runtime support:
- the review runner must support project-scoped custom subagent discovery from `.codex/agents/`
- it must also support closing and respawning those named subagents between rounds

Fallback when that support is unavailable:
- treat the matching `.codex/agents/<role>.toml` file as the reviewer contract
- emulate the role with a generic read-only agent
- keep the task prompt limited to scope, objective, and any truly local emphasis
- say explicitly in the handoff that named custom subagent loading was unavailable for that run

Concrete fallback template for a generic read-only agent:

```text
Use `.codex/agents/<role>.toml` in this repository as the full reviewer contract.
Do not edit files, run builds, spawn agents, coordinate other reviewers, or add process commentary.

Review only this scope:
<explicit scope here>

Objective of this review round:
<explicit objective here>

Local emphasis for this round:
<optional, only when truly needed>

Return ONLY the final structured report required by `.codex/agents/<role>.toml`.
```

### Review scope selection

Always state the exact review scope in the prompt.
Do not assume the agent will infer whether you want a staged diff, branch diff, or full-repo audit.

Supported review scopes include:

- staged diff
  - `review the currently staged diff in <repo-path>`
- working-tree diff
  - `review the current uncommitted diff in <repo-path>`
- branch diff
  - `review the diff between <base-ref> and <head-ref>`
- file-scoped diff
  - `review only changes in <file-paths>`
- full repo
  - `review the current repository state in <repo-path>, not just the diff`

When asking for a full-repo review, say whether you want:

- a general code-health audit
- a contract-focused audit against docs
- an architecture audit of current boundaries

### Fresh-agent rule

For review rounds, do not reuse old review agents.

- close any previous `reviewer`, `architect`, `simplifier`, and `librarian` agents first
- spawn fresh agents for the new review round
- when the project-scoped custom subagent exists, use it instead of emulating the role with a generic agent
- do this even when the previous agents already reviewed a nearby diff

### Review prompt template

Use this base template with the relevant named custom subagent.

```text
Review only this scope:
<explicit scope here>

Objective of this review round:
<explicit objective here>

Local emphasis for this round:
<optional, only when truly needed>

Return ONLY the final structured report.
```

Host-facing reviewer selection is runtime-specific.
For example, hosts that support subagent mentions may select a reviewer with a
handle such as `[@reviewer](subagent://reviewer)`.

### Completion rule

When the user has explicitly asked for a review phase, review is not complete until:

- all user-requested agents have returned a clear, usable result
- all actionable findings that the user wants addressed in that round are fixed
- and there are no remaining obvious items to clean up within the requested scope

If one or more agents still find issues and the user asked for a review loop,
keep iterating and rerun the requested reviews.
Do not start a new review round unless the user asked for that loop or asks for
another explicit review pass.

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

- `feat/<short-slug>` for feature work
- `fix/<short-slug>` for bug fixes
- `refactor/<short-slug>` for refactors
- `hardening/<short-slug>` for stabilization / recovery work

Prefer short readable slugs that match the repository's existing style, for example:

- `feat/multi-button-ble`
- `fix/wifi-reconnect-backoff`

Prefer the repository's abbreviated prefixes such as `feat/` over longer variants such as `feature/`.

Temporary Codex-generated branch names are acceptable, but stable work should converge to readable repo-style names when practical.

### PR policy

Use `branch + PR` for:

- refactors
- new components/modules
- changes in system behavior
- anything that materially affects architecture or runtime flow

Direct commits to `main` are acceptable only when explicitly aligned with the user for small, contained work.

### Issue closure

When a PR fully resolves a tracked issue:

- close the issue when the change lands in `main`, not when the release tag is created
- prefer adding `Closes #<n>` or equivalent in the PR body so GitHub closes it automatically on merge
- if the PR was merged without an auto-close reference, close the issue manually after merge and point back to the merged PR

Only wait for the release tag if the issue explicitly tracks public release availability rather than merged implementation status.

### Branch retention

- Do not delete branches after merge unless explicitly asked.

### GitHub review bot

- Always ask the user before tagging `@codex review`.

## 4. Release and tag flow

When preparing a release:

1. ensure the target branch is merged to `main`
2. ensure the release commit is final
3. create the local git tag
4. run `scripts/idf-target.sh <board> reconfigure build` for each release board profile
5. verify the embedded firmware version matches the new tag
6. push commit and tag
7. create the GitHub release
8. upload all release binaries:
   - `BluButtonBridge-esp32-devkit-v1.bin`
   - `BluButtonBridge-esp32-devkit-v1-full.bin`
   - `BluButtonBridge-esp32c3-supermini.bin`
   - `BluButtonBridge-esp32c3-supermini-full.bin`
   - `ota-manifest.json`
9. record SHA-256 checksums for all uploaded binaries
10. write GitHub release notes using the standard repo format:
   - `## Changes`
   - 2-5 concrete bullets
   - optional `## Notes` only when needed
   - use `## Notes` when the release intentionally includes a destructive compatibility break, and name the affected persisted state, config import/export schema, or upgrade expectation explicitly
   - do not repeat artifact lists or checksum blocks in the body unless the user explicitly asks for them there

Board-specific release identity is driven by `config/boards.json`.
If you add, remove, or rename a supported board/profile, update that catalog first and then verify the generated firmware, packaging scripts, and Pages workflow still agree.

Why the tag comes before the build:

- this project derives firmware version from `git describe --tags --always --dirty`
- if you build before creating the new tag, the produced firmware may still embed the previous version context
- therefore the final release build must happen after the local tag exists

### Artifact notes

- `BluButtonBridge-esp32-devkit-v1.bin` is the OTA binary for the ESP32 DevKit V1 profile
- `BluButtonBridge-esp32-devkit-v1-full.bin` is the full flash image for the ESP32 DevKit V1 profile
- `BluButtonBridge-esp32c3-supermini.bin` is the OTA binary for the ESP32-C3 SuperMini profile
- `BluButtonBridge-esp32c3-supermini-full.bin` is the full flash image for the ESP32-C3 SuperMini profile
- `ota-manifest.json` is the lightweight staged-OTA manifest consumed by the firmware before rebooting into OTA mode
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

### Console streaming changes

Test at least:

- `/console` still loads
- backlog appears on connect
- live streaming continues after backlog
- opening a second viewer replaces the first one cleanly
- the stream does not monopolize the HTTP server task

### Browser installer / Pages changes

Test at least:

- the README link points at the expected public installer URL
- the latest-release happy path still exposes a usable install button
- fallback behavior still works when live GitHub metadata cannot be fetched
- missing required release asset produces an explicit blocked-install state

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
