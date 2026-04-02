# Console Stream Contract

This document captures the stable behavior of the live web console so agents do not have to reconstruct it from `web_manager.c` and `console.html`.

## Scope

Relevant files:

- `components/web_manager/web_manager.c`
- `components/web_manager/console.html`
- `components/console_manager/console_manager.c`
- `components/console_manager/include/console_manager.h`

This contract is specifically about:

- `/console`
- `/api/console/stream`
- the in-memory serial backlog
- SSE event behavior visible to the browser

## Transport model

- `/console` serves the HTML console page
- `/api/console/stream` is an authenticated `GET` endpoint
- the stream uses `text/event-stream`
- stream work runs in a dedicated task created by `web_manager`
- the HTTP server task must not be blocked by an infinite stream loop

This is a non-JSON surface.

## Source of console lines

- `console_manager` installs a replacement `vprintf` hook
- log lines are stored in an in-memory ring buffer
- the stream reads from that ring buffer using a cursor
- the backlog is runtime-only and is not persisted across reboot

Current buffer behavior is intentionally bounded:

- only a finite backlog is retained
- older lines can be dropped when the ring buffer wraps

## SSE event contract

Current event names sent by `/api/console/stream`:

- `log`
  - normal console line payload
- `notice`
  - informational message emitted by the server about stream conditions
- `replaced`
  - sent to an older viewer when a newer viewer takes ownership

Current payload rules:

- events are emitted as standard SSE frames
- multi-line payloads are split into repeated `data:` lines
- heartbeat traffic is sent as SSE comments, not as named events

Current heartbeat behavior:

- the server sends `: keep-alive` comments roughly every 5 seconds while connected

## Single-client ownership rule

The stream is intentionally single-viewer:

- last client wins
- opening a new `/api/console/stream` increments a generation counter
- the previous stream stops being the owner
- the previous stream receives `replaced` with the message:
  - `This console was replaced by a newer viewer.`
- the previous stream then closes

If you change this behavior, update both this document and the browser client.

## Backlog and live behavior

Current model:

- a new viewer receives backlog first
- after backlog catch-up, the same stream continues with live lines
- if older backlog lines were already overwritten, the stream emits:
  - `notice`
  - `Some older log lines were dropped from the in-memory backlog.`

Important constraint:

- `Clear View` in the browser is client-side only
- it does not erase the device-side backlog

## Browser client expectations

Current browser client in `console.html` expects:

- `log` events for normal lines
- `notice` events for server-side notices
- `replaced` events to show a notice, update status, and close the `EventSource`
- the stream URL to be `/api/console/stream`

Current status text semantics in the page:

- `Connected` after `EventSource.onopen`
- `Disconnected. Reload the page to reconnect.` on generic error
- `Superseded by another process` after `replaced`

## Change guardrails

Before changing the console stream, verify:

- whether you are changing only presentation or the wire contract
- whether the browser page still understands the event names
- whether single-client ownership is still intentional
- whether the work remains outside the `httpd` handler loop

If you change:

- event names
- stream URL
- ownership semantics
- heartbeat behavior that clients depend on

update both:

- this document
- `components/web_manager/console.html`
- any related notes in `docs/RUNTIME_FLOW.md`

## Validation hints

When touching the console stream, validate at least:

- `/console` still loads
- the page connects to `/api/console/stream`
- backlog appears immediately on connect
- live lines continue after backlog
- opening a second console tab replaces the first one cleanly
- the HTTP server remains responsive while streaming
