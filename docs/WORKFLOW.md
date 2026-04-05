# Development Workflow

This document captures the working agreement for developing BluButtonBridge with coding agents.

## 1. Default development cycle

For normal feature or fix work:

1. implement the change cleanly
2. run a target-aware build+flash for the active board profile
3. let the user test on the board
4. only after successful validation proceed with commit / PR / merge

Examples:

- `source ~/esp/esp-idf/export.sh && scripts/idf-target.sh esp32-devkit-v1 flash`
- `source ~/esp/esp-idf/export.sh && ESPPORT=/dev/cu.usbmodem3101 scripts/idf-target.sh esp32c3-supermini flash`

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

When the change significantly affects repository documentation, onboarding flow, workflow guidance, or agent instructions, also run:

- `librarian`
  - reviews documentation clarity, source-of-truth hierarchy, onboarding speed, task discoverability, actionability, and AI-agent friendliness

This section applies to review-phase agents only.
It does not impose the same response format on generic agents doing implementation or exploration work.

### Review-agent output contract

Invoke each review agent with a request for structured output.
Free-form feedback is not sufficient unless it is still clearly organized into the required sections below.

Minimum shared rules for all three review agents:

- keep findings practical and actionable
- cite specific files when pointing at an issue
- prefer severity-labelled findings when reporting problems
- say explicitly when there are no material findings
- avoid generic praise or vague “looks good” responses without a verdict

The same structured-output discipline should also be used for `librarian`.

Required sections for every review agent:

- `VERDICT`
  - one of:
    - `APPROVE`
    - `APPROVE WITH NOTES`
    - `CHANGES REQUESTED`
- `FINDINGS`
  - ordered by severity
  - each finding should use `HIGH`, `MEDIUM`, or `LOW`
  - each finding should explain the concrete risk or cleanup needed
- `OPEN QUESTIONS`
  - optional when there are no open questions

If there are no material findings, require this explicitly:

- `VERDICT`
- `FINDINGS`
  - `NO MATERIAL FINDINGS`

### Review invocation discipline

When invoking review-phase agents, prompt them as single-purpose reviewers, not as coordinators.

Each review agent should be told explicitly:

- it is responsible only for its own role
- it must not coordinate or restate the three-agent workflow
- it must not spawn or suggest other review agents
- it must not discuss tool instability unless it truly cannot inspect the requested scope
- it should return only the structured review result

This matters especially when the inherited context already mentions that the repo requires
`reviewer`, `architect`, and `simplifier`. Without a local-only instruction, an agent may
misread its job and start orchestrating the whole review instead of performing its own pass.

Recommended wording:

- `You are ONLY the <role> reviewer for this change.`
- `Review only the requested scope.`
- `Do not coordinate other reviewers.`
- `Do not spawn or suggest sub-agents.`
- `Do not discuss the review process.`
- `Return only the final structured report.`

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

If you use `fork_context`, still restate the intended scope explicitly in the review prompt.

### Fresh-agent rule

For review rounds, do not reuse old review agents.

- close any previous `reviewer`, `architect`, and `simplifier` agents first
- spawn fresh agents for the new review round
- do this even when the previous agents already reviewed a nearby diff

Why this rule exists:

- it avoids stale context and cached assumptions
- it reduces the chance of agents citing old file paths or pre-fix findings
- it makes each review round easier to interpret as a clean verdict on the requested scope

### Review prompt template

Use this base template and swap in the role-specific focus and requested scope.

```text
You are ONLY the `<role>` reviewer for this change.

Review only this scope:
<explicit scope here>

Do NOT coordinate other reviewers.
Do NOT spawn or suggest sub-agents.
Do NOT discuss the review process.
Do NOT give implementation plans.
Return ONLY the final structured report.

Focus only on:
<role-specific focus here>

Use file-specific evidence from the requested scope.

Required output:

VERDICT
<APPROVE|APPROVE WITH NOTES|CHANGES REQUESTED>

FINDINGS
- <HIGH|MEDIUM|LOW>: <finding with concrete risk and file reference>
- If there are no material findings, write exactly: NO MATERIAL FINDINGS

TOP STRENGTHS
- <optional for most review roles; required for librarian>

OPEN QUESTIONS
- <optional>
```

### Role-specific expectations

`reviewer` should focus on:

- bugs
- regressions
- edge cases
- missing validation or test coverage

`architect` should focus on:

- module boundaries
- ownership
- layering
- whether the change fits the intended project structure

`simplifier` should focus on:

- duplication
- unnecessary branches or flags
- redundant state
- ways to reduce patch-on-patch complexity

`librarian` should focus on:

- clarity of documentation
- source-of-truth hierarchy
- onboarding speed for a new coding agent
- task-to-doc discoverability
- actionability of instructions
- AI-agent friendliness
- ambiguity or stale-guidance risk

For `librarian`, extend the structured output with:

- `TOP STRENGTHS`
  - 1-3 short points on what the documentation set does especially well

### Recommended scope examples

Examples:

- staged diff reviewer:
  - `Review the currently staged diff in /path/to/repo. Focus on bugs, regressions, edge cases, and missing validation.`
- branch diff architect:
  - `Review the diff between origin/main and HEAD in /path/to/repo. Focus on module boundaries, ownership, and layering.`
- full repo simplifier:
  - `Review the current repository state in /path/to/repo, not just the diff. Focus on duplication, redundant state, and complexity that can be reduced.`
- full repo librarian:
  - `Review the current documentation set in /path/to/repo from the perspective of a new coding agent. Focus on clarity, source-of-truth hierarchy, onboarding speed, task discoverability, actionability, and AI-agent friendliness.`

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
