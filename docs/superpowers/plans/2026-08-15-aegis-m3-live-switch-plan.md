# Implementation plan: Aegis M3 — Live Switch

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Goal scope:** Concurrent authorization over TCP, plus a bare Widgets window that shows one live counter
**Depends on:** [M2 Ledger](2026-08-15-aegis-m2-ledger-plan.md)
**Next:** [M4 Performance Arc](2026-08-15-aegis-m4-performance-plan.md)

## Summary

Ship end-to-end `0100`→`0110` over TCP (2-byte length prefix): acceptor, I/O threads, worker pool, **one ledger writer** (stage 1 mutex or dedicated writer thread — not striped/2PC yet), screening, reserve-then-wait `issuersim`, backpressure `96`. Ship `aegisd`, a minimal `aegis-load`, and a **bare** `aegis-console` with one live counter at ~30 Hz. `aegis` library still has **zero Qt**. Out of scope: full console, Vue, WebEngine, ledger stages 2–3, observer API freeze (a tiny counter hook is enough).

## Discovery notes

- Reuse: M1 `parse` / `serialise` and `Message`; M2 `Ledger::{reserve,capture,reverse,expire_due}`, `load_genesis`, `IdempotencyStore`.
- Constraints: hand-rolled sockets (Winsock on Windows, portable enough for Linux CI); `std::jthread`; bounded MPMC queue; engine tests without a display.
- Patterns to follow: reserve then `issuersim`; `51` / screening never call the network; timeout / `05` reverses the Hold.
- Anti-goals: no Boost.Asio; no Qt inside `aegis/`; no full KPI dashboard.

## File map

### Goal: Live switch

| Path | Create/Modify | Responsibility | Public surface | Verified/Provisional |
|------|---------------|----------------|----------------|----------------------|
| `aegis/net/socket.hpp` | create | RAII socket, listener, connection | `Listener`, `Connection` | provisional |
| `aegis/net/framing.hpp` | create | 2-byte length prefix read/write | `read_frame`, `write_frame` | provisional |
| `aegis/concurrent/bounded_queue.hpp` | create | Bounded MPMC queue | `push` fails if full; `pop` | provisional |
| `aegis/concurrent/thread_pool.hpp` | create | Workers + `stop_token` | `submit`, shutdown | provisional |
| `aegis/issuersim/issuersim.hpp` | create | Sleep/latency, timeout, inject `05`; **no balances** | `await_decision` | provisional |
| `aegis/authorizer/screen.hpp` | create | Hard rules: currency, amount>0, optional max | `screen` → `Result` | provisional |
| `aegis/authorizer/authorizer.hpp` | create | Validate, screen, funds/reserve, wait issuer, reverse on fail | `handle(Message)` | provisional |
| `aegis/engine/engine.hpp` | create | Accept loop, I/O, workers, single ledger writer, start/stop | `Engine` | provisional |
| `aegis/metrics/counter.hpp` | create | Atomic auth count (enough for the bare window) | `snapshot_count` | provisional |
| `apps/aegisd/main.cpp` | create | Headless: bind port, load genesis, run until signal | CLI | provisional |
| `apps/aegis-load/main.cpp` | create | Minimal: N connections, send 0100, print 0110 | CLI | provisional |
| `apps/aegis-console/CMakeLists.txt` | create | Qt Widgets target; **does not** link into `aegis` | target `aegis-console` | provisional |
| `apps/aegis-console/main.cpp` | create | QTimer ~30 Hz, one QLabel counter; engine on background threads | — | provisional |
| `CMakeLists.txt` | modify | Optional Qt Widgets for the console target only | feature or optional component | verified |
| `vcpkg.json` | modify | Optional Qt Widgets; still no WebEngine | manifest feature | verified |
| `tests/framing_test.cpp` | create | Length prefix round-trip, truncated frame error | — | provisional |
| `tests/queue_backpressure_test.cpp` | create | Full queue fails push (maps to 96 at the engine) | — | provisional |
| `tests/authorizer_test.cpp` | create | Screen decline, 51 with no issuer call, reserve then timeout reverses | — | provisional |
| `tests/engine_tcp_test.cpp` | create | Loopback 0100/0110 | — | provisional |

### Blast radius

| Path | Why sensitive | Behavior that must stay intact |
|------|----------------|--------------------------------|
| `aegis/engine/engine.hpp` | Process-wide threading; easy to leak Qt or globals | `aegis` headers include no Qt. One ledger writer. No process-wide mutable state except the `Engine` instance |
| `aegis/net/socket.hpp` | Winsock vs POSIX | Tests use loopback. Linux CI and Windows both accept a connection and exchange one frame |
| `apps/aegis-console/*` | First Qt; must not infect `aegis` | `aegis_tests` stays headless. The console target is optional |

## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition).
2. Per subtask: **Plan mode** + **planning-subtasks** → **CreatePlan** type-2 plan when **Plan mode** is `high` (or `medium` with a real remaining unknown).
3. **Agent mode**: **test-driven-development** when **`TDD suitable: yes`** (or the TDD slice of **`partial`**); follow **Verification** when **`no`**.
4. Equivalent internal changes: record and continue. Material changes: pause and confirm, then log a **Plan changelog** row.

## Subtasks

Dependency notation: `Blocked by: T1` means start after T1 is done.

### T1 — Framing + RAII sockets (loopback)

- [ ] **Do:** 2-byte length prefix; RAII listener/connection; unit-test framing without threads.
- **Consumes:** M1 `std::span<const std::byte>` style for payloads (codec stays behind the frame)
- **Produces:** `read_frame` / `write_frame` over a connection; `Listener` bind/accept; `Connection` RAII close
- **Acceptance:** A frame round-trips including an empty payload. A truncated length or body returns an error and does not block forever in the unit test. Loopback accept + one write/read works on the host OS.
- **Compatibility:** The on-wire prefix is a 2-byte length of the ISO body. M4 load generation uses the same framing.
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `framing_test`; optional loopback smoke.

### T2 — Bounded queue + thread pool

- [ ] **Do:** Bounded MPMC; `jthread` pool with cooperative stop. Full `push` fails (no unbounded growth).
- **Consumes:** none from T1 (can land in parallel)
- **Produces:** `BoundedQueue::push` → fail when full; `pop`; `ThreadPool::submit` and shutdown via `stop_token`
- **Acceptance:** Push past capacity fails and the queue size stays at the cap. N submitted jobs all run. Shutdown returns and does not leave joinable threads.
- **Blocked by:** —
- **Plan mode:** medium — named unknown: mutex + condition variable versus a lock-free queue. The fail-on-full contract is already fixed; pick one and keep the test.
- **TDD suitable:** yes
- **Verification:** `queue_backpressure_test`; pool runs N jobs then stops.

### T3 — Screening + issuersim + authorizer (in-process)

- [ ] **Do:** Hard screen; issuersim delay/timeout/`05`; authorizer: validate → screen → `51` or reserve → wait → `00` or reverse.
- **Consumes:** M2 `Ledger::reserve` / `reverse`, `IdempotencyStore`; M1 `Message` and response field 39
- **Produces:** `screen(Message) -> Result`; `await_decision` (no ledger access); `handle(Message) -> Message` response
- **Acceptance:** Screening decline and `InsufficientFunds` do not call `issuersim`. A successful path reserves, waits, and returns `00`. Issuer timeout or injected `05` reverses the Hold and does not leave it live. Duplicate key returns the stored reserve outcome.
- **Compatibility:** `issuersim` never reads or writes balances. Response codes stay the spec’s `00` / `05` / `51`.
- **Blocked by:** M2 ledger (done)
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `authorizer_test` — 30/05/51/00/timeout-reverse; fake clock for issuer wait.

### T4 — Engine: acceptor, I/O, workers, single ledger writer, 96

- [ ] **Do:** Wire topology; full inbound queue → respond `96`; genesis on start.
- **Consumes:** T1 framing and sockets; T2 queue and pool; T3 `handle`
- **Produces:** `Engine` start/stop; one ledger writer; inbound overflow responds with field 39 = `96`; `snapshot_count` atomic
- **Acceptance:** Loopback client sends `0100` and receives `0110`. When the inbound queue is full, the response code is `96` and memory does not grow with the rejected load. Stop joins the threads.
- **Compatibility:** Stage 1 stays a single writer. Striped locks and 2PC are M4 and must not appear here.
- **Blocked by:** T1, T2, T3
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `engine_tcp_test` loopback 0100/0110; backpressure test forces 96.

### T5 — `aegisd` + `aegis-load` CLIs

- [ ] **Do:** Headless daemon + load generator sending real TCP.
- **Consumes:** T4 `Engine`, `load_genesis`
- **Produces:** `aegisd` (port, genesis path, run until signal); `aegis-load` (host, port, connection count, one `0100` each, print `0110`)
- **Acceptance:** Start `aegisd`, run `aegis-load`, and see approval responses. No new authorization rules beyond T4.
- **Blocked by:** T4
- **Plan mode:** skip
- **TDD suitable:** partial
- **TDD suitable reason:** CLI wiring; behavior already locked in engine tests. Smoke is manual/integration.
- **Verification:** Manual or CI script: start `aegisd`, run `aegis-load`, see approvals.

### T6 — Bare Qt window (live counter only)

- [ ] **Do:** Optional `aegis-console` linking Qt Widgets + `aegis`; QTimer ~30 Hz reads the atomic count. Engine has no Qt.
- **Consumes:** T4 `snapshot_count`
- **Produces:** target `aegis-console`; one window whose label tracks the auth count
- **Acceptance:** While `aegis-load` runs, the label changes about every 30 ms of timer ticks. `aegis` sources do not include Qt. `aegis_tests` still passes with no display.
- **Compatibility:** This window is not the M5 dashboard. Do not add KPIs, charts, or WebEngine here.
- **Blocked by:** T4
- **Plan mode:** medium — named unknown: how the console target optionally links Qt Widgets so a machine without Qt still builds `aegis` and `aegis_tests`.
- **TDD suitable:** no
- **TDD suitable reason:** purely visual/wiring first window; no deterministic UI core yet. Headless tests must still pass without Qt.
- **Verification:** Manual: window updates while load runs. `aegis_tests` job does not require a display.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**: **`yes`** means strict **test-driven-development** (red/green/refactor); **`partial`** applies it only to the testable slice; **`no`** means do not force test-first — still satisfy **Verification**. A behavior-preserving refactor with a covering suite is **`no`** (stay-green), not a new RED/GREEN pair. Type-2 planning → **planning-subtasks** skill. UI work is often **`partial`**: TDD the behavior core, verify polish and motion manually.

T1–T4 are **`yes`**. T5 is **`partial`** (smoke CLIs). T6 is **`no`**.

## Plan changelog

| Date | Change | Kind |
|------|--------|------|
| 2026-08-15 | Initial plan | — |
| 2026-10-01 | Reshaped to the updated writing-plans scaffold. Plan mode retagged by missing decisions: T2 and T6 are `medium` with a named unknown; T5 is `skip`. T1, T3, T4 stay `high`. TDD tags unchanged | equivalent |
