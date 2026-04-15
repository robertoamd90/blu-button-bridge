# AGENTS.md

Repository-wide instructions for coding agents working on BluButtonBridge.

## Read this first

Before doing non-trivial work, read:

- [README.md](README.md)
- [docs/WORKFLOW.md](docs/WORKFLOW.md)
- [docs/API_CONTRACTS.md](docs/API_CONTRACTS.md)
- [docs/CONSOLE_STREAM_CONTRACT.md](docs/CONSOLE_STREAM_CONTRACT.md)
- [docs/PAGES_INSTALLER_CONTRACT.md](docs/PAGES_INSTALLER_CONTRACT.md)
- [docs/RUNTIME_FLOW.md](docs/RUNTIME_FLOW.md)
- [docs/VALIDATION.md](docs/VALIDATION.md)
- [docs/WHERE_TO_START.md](docs/WHERE_TO_START.md)

For project-scoped Codex runtime settings, also read:

- `.codex/config.toml`

For review-phase Codex subagent work, also read:

- `.codex/agents/*.toml`

## Document roles

Use the docs with these responsibilities in mind:

- `AGENTS.md`
  - repo policy and agent-specific guardrails
- `docs/WORKFLOW.md`
  - development cycle, release flow, and review-agent invocation contract
- `.codex/config.toml`
  - project-scoped subagent runtime settings
- `.codex/agents/*.toml`
  - source of truth for reviewer identity, mandate, forbidden actions, and output contract
- `docs/VALIDATION.md`
  - evidence ladder and fallback behavior when full validation is blocked
- `docs/API_CONTRACTS.md`
  - source of truth for API payloads, config schema, and compatibility-sensitive contracts
- `docs/CONSOLE_STREAM_CONTRACT.md`
  - source of truth for the console SSE stream contract and browser expectations
- `docs/PAGES_INSTALLER_CONTRACT.md`
  - source of truth for the GitHub Pages installer payload and mirror behavior
- `docs/RUNTIME_FLOW.md`
  - source of truth for boot flow, runtime ownership, and cross-module interactions
- `docs/WHERE_TO_START.md`
  - fast entry points for common tasks

If documents overlap or appear to conflict:

- `README.md` is overview and operator-facing orientation
- `docs/WORKFLOW.md` wins for development/review process and review-agent expectations
- `.codex/config.toml` is runtime configuration only and does not participate in prose-doc conflict resolution
- `.codex/agents/*.toml` win for reviewer-role definition and reviewer output contract
- `docs/API_CONTRACTS.md` wins for HTTP payloads, config schema, and compatibility-sensitive API details
- `docs/CONSOLE_STREAM_CONTRACT.md` wins for `/console`, `/api/console/stream`, SSE semantics, and console-browser behavior
- `docs/PAGES_INSTALLER_CONTRACT.md` wins for the GitHub Pages installer, mirrored firmware payload, and browser install flow
- `docs/RUNTIME_FLOW.md` wins for boot flow, ownership, and cross-module runtime behavior
- `docs/VALIDATION.md` wins for validation expectations and fallback behavior
- `AGENTS.md` wins for repository policy and agent process

## Core rules

- Prefer small, clean changes over layered patches.
- Stop and realign when a change starts bending module boundaries or introducing ad hoc flags.
- Keep `app_main()` linear and lightweight.
- Put domain logic in the owning module instead of in transport or bootstrap code.
- If you add, remove, or rename an HTTP route in `components/web_manager/web_manager.c`, update the relevant contract documentation before considering the work complete.
  - Use `docs/API_CONTRACTS.md` for JSON/API payloads and compatibility-sensitive HTTP routes.
  - Use `docs/CONSOLE_STREAM_CONTRACT.md` for `/console` and `/api/console/stream`.
  - Use `docs/PAGES_INSTALLER_CONTRACT.md` for the browser installer and mirrored Pages payload.
- Do not assume backward compatibility for intentionally destructive persisted-state or schema changes.
  - This repository may ship destructive upgrades without migration when that is the explicit choice for the change.
  - When a release intentionally breaks persisted state compatibility, call it out explicitly in the release notes.

## Validation

- Follow [docs/VALIDATION.md](docs/VALIDATION.md) for the validation ladder and fallback rules.
- For non-trivial work, the default validation step is a target-aware build+flash for the active board profile, for example:
  - `source ~/esp/esp-idf/export.sh && scripts/idf-target.sh esp32-devkit-v1 flash`
  - `source ~/esp/esp-idf/export.sh && ESPPORT=/dev/cu.usbmodem3101 scripts/idf-target.sh esp32c3-supermini flash`
- After flashing, stop and let the user run on-device tests unless explicitly asked to do more.

## User-directed review agents

Review-phase agents are not invoked automatically.
The user must explicitly say when to start review work and which review round
they want.

Default rule:

- do not call `reviewer`, `architect`, `simplifier`, or `librarian` unless the
  user explicitly asks for review agents in the current task
- do not start or continue a `fix -> review -> fix -> review` loop unless the
  user explicitly asks for that loop in the current task

When the user explicitly requests a review round for non-trivial work, the
default review set is:

- `reviewer`
- `architect`
- `simplifier`

When the user explicitly requests a review round and the change significantly
affects repository documentation, onboarding flow, workflow guidance, or agent
instructions, also run:

- `librarian`

Outside an explicit user-requested review phase, work may be completed with
implementation plus validation only. In that case, say clearly in the handoff
that review agents were not run because the user did not request them.

The operational custom subagent definitions for review roles live in:

- `.codex/agents/reviewer.toml`
- `.codex/agents/architect.toml`
- `.codex/agents/simplifier.toml`
- `.codex/agents/librarian.toml`

Treat `.codex/agents/*.toml` as the source of truth for reviewer identity,
mandate, forbidden actions, and output format.

When invoking these review agents, follow the review invocation contract in
`docs/WORKFLOW.md` and keep `AGENTS.md` at the policy level only.

When the user has explicitly asked for a review phase, review is not complete
until:

- all user-requested agents have produced a clear, usable outcome
- all actionable findings that the user wants addressed in that round are fixed
- and no obvious cleanup remains within the requested review scope

If an agent still finds issues and the user asked for a review loop, keep
iterating and rerun the requested reviews.
If tooling is unstable, say that explicitly instead of claiming review is complete.

## Git / PR rules

- Do not delete merged branches unless the user explicitly asks.
- Ask before tagging `@codex review` on GitHub PRs.
- Use `branch + PR` for refactors, new modules, behavior changes, and structurally meaningful work.
- Small targeted fixes may go directly to `main` only when clearly agreed.
- When work fully resolves a tracked issue, close it at merge time. Prefer PR auto-close references such as `Closes #<n>` and do not wait for the release tag unless the issue explicitly tracks release availability.

## Release rules

- Follow [docs/WORKFLOW.md](docs/WORKFLOW.md) for the full release flow.
- Create the local release tag before the final release build.
- Run final release builds through `scripts/idf-target.sh <board> reconfigure build` for each release board profile.
- Release artifacts must include all board-specific OTA and full images.
- Report SHA-256 for all released artifacts.
- Keep `dist/` untracked unless explicitly requested otherwise.
