# Implementation plan: Aegis M4 — Performance Arc

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**ADR:** [0002 capture is cross-wallet](../../adr/0002-capture-is-cross-wallet.md)
**Created:** 2026-08-15
**Subsystem scope:** Ledger stages 2–3, benchmarks, Qt-free observer API
**Depends on:** [M3 Live Switch](2026-08-15-aegis-m3-live-switch-plan.md)
**Next:** [M5 Dual Consoles](2026-08-15-aegis-m5-dual-consoles-plan.md)

## Summary

Measure stage 1 (M3 writer), implement striped locks (stage 2) then wallet partitions + **capture 2PC** (stage 3), record three benchmark runs with p50/p99/p99.9, freeze a Qt-free `observer` API (snapshot, lossy ring, commands), TSan in Linux CI, shadow model agrees including captures. Out of scope: full Widgets dashboard, Vue, WebEngine.

## Discovery notes

- Reuse: M2 shadow model (do not rewrite it to be “fast”); M3 engine topology; M3 atomic counter becomes metrics histogram + observer.
- Constraints: three written analyses; TSan Linux (not MSVC); capture always 2PC at stage 3; authorization stays single-wallet.
- Patterns: lock ordering on striped two-lock paths; shared-nothing partitions; observer copyable structs, no engine pointers.
- Anti-goals: do not chase latency after the three write-ups; do not put Qt in observer.

## File map

### Subsystem: Performance + observer

| Path | Create/Modify | Responsibility | Public surface |
|------|----------------|----------------|----------------|
| `aegis/ledger/ledger.hpp` | modify | Strategy or compile-time/runtime stage switch | stage 1 remains default until measured |
| `aegis/ledger/striped.hpp` | create | Stage 2: hash wallet → stripe lock; ordered lock acquire | internal |
| `aegis/ledger/partition.hpp` | create | Stage 3: one thread per wallet partition | internal |
| `aegis/ledger/twophase.hpp` | create | Capture 2PC across Cardholder/Merchant/System partitions | `capture` coordination |
| `aegis/metrics/histogram.hpp` | create | Latency histogram, throughput | record, percentiles |
| `aegis/observer/snapshot.hpp` | create | Copyable `MetricsSnapshot` | struct fields per spec |
| `aegis/observer/ring.hpp` | create | Bounded lossy transaction ring | `push`, `drain` |
| `aegis/observer/commands.hpp` | create | start/stop, inject slow issuer / timeouts / drop | `ObserverCommands` |
| `aegis/observer/observer.hpp` | create | Facade used by GUIs and `aegisd` websocket later | `snapshot()`, `drain()`, commands |
| `apps/aegis-load/main.cpp` | modify | Configurable rate, duration, terminal count | CLI flags |
| `apps/aegisd/main.cpp` | modify | Benchmark mode: print p50/p99/p99.9 + throughput | `--bench` |
| `bench/results/.gitkeep` | create | Committed result files | — |
| `bench/RESULTS.md` | create | Three runs + why each change helped | markdown |
| `tests/ledger_stage2_test.cpp` | create | Parallel unrelated wallets; lock-order transfer/capture if stage 2 capture uses two locks | — |
| `tests/ledger_stage3_2pc_test.cpp` | create | Capture 2PC vs shadow; failure rolls back | — |
| `tests/observer_test.cpp` | create | Snapshot copy; ring overwrite when full; commands don’t leak pointers | — |
| `.github/workflows/ci.yml` | modify | TSan job, concurrent smoke | — |

### Blast radius

| Path | Why sensitive | Plan mode (before implementation) |
|------|----------------|-----------------------------------|
| `aegis/ledger/*` stage 3 | Money + 2PC; easy to desync shadow | high — 2PC protocol on paper before code |
| `aegis/observer/*` | M5 GUI contract; changing later breaks both UIs | high — freeze snapshot/command fields |
| Engine thread topology | False sharing / races | high — TSan required before calling M4 done |

## Workflow (for implementers)

1. Type-1 (this file).
2. Type-2 especially for 2PC and observer freeze.
3. Agent + TDD.
4. Changelog if observer fields change (M5 must follow).

## Subtasks

### T1 — Histogram + `aegisd --bench` + `aegis-load` rate

- [ ] **Do:** Record latency/throughput; configurable load; write stage-1 numbers to `bench/`.
- **Blocked by:** —
- **Plan mode:** medium
- **TDD suitable:** partial
- **TDD suitable reason:** histogram math is TDD; CLI/benchmark harness is measurement/checklist.
- **Verification:** Histogram unit tests; one committed stage-1 result file.

### T2 — Observer API (Qt-free)

- [ ] **Do:** `MetricsSnapshot`, lossy ring, start/stop + three fault injects; no pointers into engine.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `observer_test`; `aegis` headers compile without Qt.

### T3 — Ledger stage 2 striped locks

- [ ] **Do:** Hash wallets to N locks; ordered acquire; measure; write analysis vs stage 1.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** Stage2 tests; shadow agrees; `bench/RESULTS.md` stage 2 section.

### T4 — Ledger stage 3 partitions + capture 2PC

- [ ] **Do:** One writer thread per wallet partition; auth intra-partition; capture 2PC Cardholder+Merchant+System; shadow agrees under load.
- **Blocked by:** T3
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `ledger_stage3_2pc_test`; load vs shadow; stage 3 bench + write-up.

### T5 — ThreadSanitizer CI smoke

- [ ] **Do:** Linux Clang TSan job on concurrent engine smoke (auth+capture mix).
- **Blocked by:** T4
- **Plan mode:** medium
- **TDD suitable:** no
- **TDD suitable reason:** CI config; races are found by TSan run, not gtest-first.
- **Verification:** TSan job green; RESULTS.md complete (three runs).

## TDD note (Agent mode)

T2–T4 **`yes`**. T1 **`partial`**. T5 **`no`**.

## Plan changelog

| Date | Change |
|------|--------|
| 2026-08-15 | Initial plan |
