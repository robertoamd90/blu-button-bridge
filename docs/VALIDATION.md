# Validation Ladder

This document defines what evidence is expected before claiming work is done.

## 1. Default rule

For non-trivial work, the normal validation path is:

1. run `idf.py -p /dev/cu.usbserial-0001 build flash`
2. let the user run on-device validation
3. only then move to commit / PR / merge

This is the preferred path because it validates both:

- compilation
- deployment to the actual board

## 2. When the board is available

Minimum expected evidence:

- `build flash` succeeds
- the user can test the changed behavior on the board

When the change touches a specific area, validate at least:

### OTA

- GitHub OTA success path
- GitHub OTA failure path
- recovery to normal boot after failure
- manual upload still works

### Web UI

- no obvious layout regressions
- refresh behavior
- any newly added persistence behavior
- dark/light handling if touched

### Runtime / connectivity

- boot still completes
- reconnect behavior still makes sense
- status reporting matches behavior
- LED policy still matches runtime state if affected

## 3. When the board is not available

Fallback evidence:

1. run `idf.py build`
2. verify the critical code path by inspection
3. state explicitly that hardware validation is still missing

Do not phrase this as “verified” without qualifiers.
Say instead:

- build passes
- code path reviewed
- hardware validation still pending

## 4. When the serial port is unavailable

If the board exists but `/dev/cu.usbserial-0001` is busy or unavailable:

1. say that flashing could not be completed
2. still run `idf.py build`
3. report the blocked validation clearly

Do not silently downgrade this to “done”.

## 5. When external dependencies are unavailable

Examples:

- GitHub unreachable
- broker unreachable
- local DNS unavailable
- router/firewall unavailable

Fallback rule:

1. validate what is local and deterministic
2. identify exactly which external dependency prevented full validation
3. report the residual risk attached to that missing dependency

Examples:

- “OTA staging validated locally; end-to-end GitHub download still blocked by network access”
- “MQTT reconnect logic reviewed and built; broker-side validation still pending”

## 6. Review requirements

For non-trivial work, validation also includes the multi-agent review required by:

- [AGENTS.md](../AGENTS.md)
- [docs/WORKFLOW.md](WORKFLOW.md)

This document focuses on validation fallback and evidence quality, not on redefining the full review process.

## 7. Tool instability rule

If sub-agent tooling is unstable:

- say so explicitly
- do not claim the review is complete
- distinguish clearly between:
  - build confidence
  - static review confidence
  - hardware confidence

## 8. Reporting template

When validation is partial, report it in this order:

1. what was validated successfully
2. what could not be validated
3. why it could not be validated
4. what residual risk remains
