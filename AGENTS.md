# AGENTS.md

Repository-wide instructions for coding agents working on BluButtonBridge.

## Read this first

Before doing non-trivial work, read:

- [docs/WORKFLOW.md](docs/WORKFLOW.md)
- [docs/API_CONTRACTS.md](docs/API_CONTRACTS.md)
- [docs/RUNTIME_FLOW.md](docs/RUNTIME_FLOW.md)
- [docs/VALIDATION.md](docs/VALIDATION.md)
- [docs/WHERE_TO_START.md](docs/WHERE_TO_START.md)

## Document roles

Use the docs with these responsibilities in mind:

- `AGENTS.md`
  - repo policy and agent-specific guardrails
- `docs/WORKFLOW.md`
  - development cycle, release flow, and testing expectations
- `docs/VALIDATION.md`
  - evidence ladder and fallback behavior when full validation is blocked
- `docs/API_CONTRACTS.md`
  - source of truth for API payloads, config schema, and compatibility-sensitive contracts
- `docs/RUNTIME_FLOW.md`
  - source of truth for boot flow, runtime ownership, and cross-module interactions
- `docs/WHERE_TO_START.md`
  - fast entry points for common tasks

If documents overlap or appear to conflict:

- `README.md` is overview and operator-facing orientation
- `docs/API_CONTRACTS.md` wins for HTTP payloads, config schema, and compatibility-sensitive API details
- `docs/RUNTIME_FLOW.md` wins for boot flow, ownership, and cross-module runtime behavior
- `docs/WORKFLOW.md` and `docs/VALIDATION.md` win for release and validation expectations

## Core rules

- Prefer small, clean changes over layered patches.
- Stop and realign when a change starts bending module boundaries or introducing ad hoc flags.
- Keep `app_main()` linear and lightweight.
- Put domain logic in the owning module instead of in transport or bootstrap code.
- If you add, remove, or rename an HTTP route in `components/web_manager/web_manager.c`, update the relevant API documentation in `docs/API_CONTRACTS.md` before considering the work complete.

## Validation

- Follow [docs/VALIDATION.md](docs/VALIDATION.md) for the validation ladder and fallback rules.
- For non-trivial work, the default validation step is `idf.py -p /dev/cu.usbserial-0001 build flash`.
- After flashing, stop and let the user run on-device tests unless explicitly asked to do more.

## Mandatory multi-agent review

For non-trivial changes, run all three reviews:

- `reviewer`
- `architect`
- `simplifier`

This requirement is specifically for the review phase of the workflow.
It does not redefine how generic work agents should be prompted for normal implementation, exploration, or research tasks.

When invoking these review agents, ask for a structured response format as defined in [docs/WORKFLOW.md](docs/WORKFLOW.md).

Review is not complete until:

- all three agents have produced a clear, usable outcome
- all actionable findings are fixed
- and no obvious cleanup remains

If an agent still finds issues, keep iterating and rerun the reviews.
If tooling is unstable, say that explicitly instead of claiming review is complete.

## Git / PR rules

- Do not delete merged branches unless the user explicitly asks.
- Ask before tagging `@codex review` on GitHub PRs.
- Use `branch + PR` for refactors, new modules, behavior changes, and structurally meaningful work.
- Small targeted fixes may go directly to `main` only when clearly agreed.

## Release rules

- Follow [docs/WORKFLOW.md](docs/WORKFLOW.md) for the full release flow.
- Create the local release tag before the final release build.
- Run `idf.py reconfigure build` for the final release build.
- Release artifacts must include `BluButtonBridge.bin` and `BluButtonBridge-full.bin`.
- Report SHA-256 for both artifacts.
- Keep `dist/` untracked unless explicitly requested otherwise.
