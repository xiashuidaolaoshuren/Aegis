# Implementation plan: Aegis M3 — Live Switch

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Subsystem scope:** Concurrent engine over TCP + bare Widgets window
**Depends on:** [M2 Ledger](2026-08-15-aegis-m2-ledger-plan.md)
**Next:** [M4 Performance Arc](2026-08-15-aegis-m4-performance-plan.md)

## Summary

Ship end-to-end `0100`→`0110` over TCP (2-byte length prefix): acceptor, I/O threads, worker pool, **one ledger writer** (stage 1 mutex or dedicated writer thread — not striped/2PC yet), screening, reserve-then-wait `issuersim`, backpressure `96`. Ship `aegisd`, a minimal `aegis-load`, and a **bare** `aegis-console` with one live counter at ~30 Hz. `aegis` library still has **zero Qt**. Out of scope: full console, Vue, WebEngine, ledger stages 2–3, observer API freeze (a tiny counter hook is enough).

## Discovery notes

- Reuse: M1 codec, M2 ledger (reserve/capture/reverse/expire, genesis, idempotency).
- Constraints: hand-rolled sockets (Winsock on Windows, portable enough for Linux CI); `std::jthread`; bounded MPMC queue; engine tests without a display.
- Patterns: reserve then `issuersim`; `51`/`screening` never call network; timeout/`05` reverse Hold.
- Anti-goals: no Boost.Asio; no Qt inside `aegis/`; no full KPI dashboard.

## File map

### Subsystem: Live Switch

| Path | Create/Modify | Responsibility | Public surface |
|------|----------------|----------------|----------------|
| `aegis/net/socket.hpp` | create | RAII socket, listener, connection | `Listener`, `Connection` |
| `aegis/net/framing.hpp` | create | 2-byte length prefix read/write | `read_frame`, `write_frame` |
| `aegis/concurrent/bounded_queue.hpp` | create | Bounded MPMC queue | `push` (fail if full), `pop` |
| `aegis/concurrent/thread_pool.hpp` | create | Workers + `stop_token` | `submit`, shutdown |
| `aegis/issuersim/issuersim.hpp` | create | Sleep/latency, timeout, inject `05`; **no balances** | `await_decision` |
| `aegis/authorizer/screen.hpp` | create | Hard rules: currency, amount>0, optional max | `screen` → Result |
| `aegis/authorizer/authorizer.hpp` | create | Validate, screen, funds/reserve, wait issuer, reverse on fail | `handle(Message)` |
| `aegis/engine/engine.hpp` | create | Accept loop, I/O, workers, single ledger writer, start/stop | `Engine` |
| `aegis/metrics/counter.hpp` | create | Atomic auth count (enough for bare window) | `snapshot_count` |
| `apps/aegisd/main.cpp` | create | Headless: bind port, load genesis, run until signal | CLI |
| `apps/aegis-load/main.cpp` | create | Minimal: N connections, send 0100, print 0110 | CLI |
| `apps/aegis-console/CMakeLists.txt` | create | Qt Widgets target; **does not** link into `aegis` | target `aegis-console` |
| `apps/aegis-console/main.cpp` | create | QTimer 30 Hz, one QLabel counter; engine on background threads | — |
| `CMakeLists.txt` / `vcpkg.json` | modify | Optional Qt Widgets for console target only | feature or optional component |
| `tests/framing_test.cpp` | create | Length prefix round-trip, truncated frame error | — |
| `tests/queue_backpressure_test.cpp` | create | Full queue fails push (maps to 96 at engine) | — |
| `tests/authorizer_test.cpp` | create | Screen 05, 51 no issuer call, reserve then timeout reverses | — |
| `tests/engine_tcp_test.cpp` | create | Loopback 0100/0110 | — |

### Blast radius

| Path | Why sensitive | Plan mode (before implementation) |
|------|----------------|-----------------------------------|
| `aegis/engine/engine.hpp` | Process-wide threading; easy to leak Qt or globals | high — no Qt includes; no process-wide mutable except engine instance |
| `aegis/net/socket.hpp` | Winsock vs POSIX | high — thin portable wrapper, tests on loopback |
| `apps/aegis-console/*` | First Qt; must not infect `aegis` | high — CMake: console optional; `aegis_tests` still headless |

## Workflow (for implementers)

1. Type-1 (this file).
2. Type-2 for high/medium subtasks.
3. Agent + TDD tags.
4. Changelog if topology changes.

## Subtasks

### T1 — Framing + RAII sockets (loopback)

- [ ] **Do:** 2-byte length prefix; RAII listener/connection; unit-test framing without threads.
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `framing_test`; optional loopback smoke.

### T2 — Bounded queue + thread pool

- [ ] **Do:** Bounded MPMC; `jthread` pool with cooperative stop. Full `push` fails (no unbounded growth).
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `queue_backpressure_test`; pool runs N jobs then stops.

### T3 — Screening + issuersim + authorizer (in-process)

- [ ] **Do:** Hard screen; issuersim delay/timeout/`05`; authorizer: validate→screen→51 or reserve→wait→00 or reverse.
- **Blocked by:** M2 ledger
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `authorizer_test` — 30/05/51/00/timeout-reverse; mock or fake clock for issuer wait.

### T4 — Engine: acceptor, I/O, workers, single ledger writer, 96

- [ ] **Do:** Wire topology; full inbound queue → respond `96`; genesis on start.
- **Blocked by:** T1, T2, T3
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `engine_tcp_test` loopback 0100/0110; backpressure test forces 96.

### T5 — `aegisd` + `aegis-load` CLIs

- [ ] **Do:** Headless daemon + load generator sending real TCP.
- **Blocked by:** T4
- **Plan mode:** medium
- **TDD suitable:** partial
- **TDD suitable reason:** CLI wiring; behavior already locked in engine tests. Smoke is manual/integration.
- **Verification:** Manual/CI script: start `aegisd`, run `aegis-load`, see approvals.

### T6 — Bare Qt window (live counter only)

- [ ] **Do:** Optional `aegis-console` linking Qt Widgets + `aegis`; QTimer 30 Hz reads atomic count. Engine has no Qt.
- **Blocked by:** T4
- **Plan mode:** medium
- **TDD suitable:** no
- **TDD suitable reason:** purely visual/wiring first window; no deterministic UI core yet. Headless tests must still pass without Qt.
- **Verification:** Manual: window updates while load runs. `aegis_tests` job does not require display.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**. T6 is **`no`** (bare visual). T5 is **`partial`** (smoke CLIs).

## Plan changelog

| Date | Change |
|------|--------|
| 2026-08-15 | Initial plan |
