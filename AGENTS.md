# AGENTS.md

Repository-wide instructions for coding agents working on BluButtonBridge.

## Read this first

Before doing non-trivial work, read:

- [docs/WORKFLOW.md](docs/WORKFLOW.md)
- [docs/API_CONTRACTS.md](docs/API_CONTRACTS.md)
- [docs/RUNTIME_FLOW.md](docs/RUNTIME_FLOW.md)
- [docs/VALIDATION.md](docs/VALIDATION.md)
- [docs/WHERE_TO_START.md](docs/WHERE_TO_START.md)

## Core rules

- Prefer small, clean changes over layered patches.
- Stop and realign when a change starts bending module boundaries or introducing ad hoc flags.
- Keep `app_main()` linear and lightweight.
- Put domain logic in the owning module instead of in transport or bootstrap code.

## Validation

- For non-trivial work, the default validation step is:
  - `idf.py -p /dev/cu.usbserial-0001 build flash`
- After flashing, stop and let the user run on-device tests unless explicitly asked to do more.
- If board access or external dependencies are unavailable, follow the fallback ladder in [docs/VALIDATION.md](docs/VALIDATION.md).

## Mandatory multi-agent review

For non-trivial changes, run all three reviews:

- `reviewer`
- `architect`
- `simplifier`

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

- Create the local release tag before the final release build.
- Run `idf.py reconfigure build` for the final release build.
- Release artifacts must include:
  - `BluButtonBridge.bin`
  - `BluButtonBridge-full.bin`
- Report SHA-256 for both artifacts.
- Keep `dist/` untracked unless explicitly requested otherwise.
- GitHub release notes should use a short, uniform format:
  - default to a single `## Changes` section
  - use 2-5 concrete bullets describing user-visible or structurally important changes
  - avoid vague summaries like "latest fixes" or repeating artifact/checksum lists already visible in the release UI
  - add `## Notes` only when a release needs a short warning, migration note, or exceptional caveat
