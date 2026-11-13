# Implementation plan: Aegis M4 — Performance Arc

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**ADR:** [0002 capture is cross-wallet](../../adr/0002-capture-is-cross-wallet.md)
**Created:** 2026-08-15
**Goal scope:** Measure stage 1, ship striped locks then wallet partitions with capture 2PC, and freeze a Qt-free observer API
**Depends on:** [M3 Live Switch](2026-08-15-aegis-m3-live-switch-plan.md)
**Next:** [M5 Dual Consoles](2026-08-15-aegis-m5-dual-consoles-plan.md)

## Summary

Measure stage 1 (M3 writer), implement striped locks (stage 2) then wallet partitions + **capture 2PC** (stage 3), record three benchmark runs with p50/p99/p99.9, freeze a Qt-free `observer` API (snapshot, lossy ring, commands), TSan in Linux CI, shadow model agrees including captures. Out of scope: full Widgets dashboard, Vue, WebEngine.

## Discovery notes

- Reuse: M2 `tests/shadow_model.hpp` (do not rewrite it to be “fast”); M3 engine topology; M3 `snapshot_count` becomes the histogram + observer.
- Constraints: three written analyses; TSan on Linux Clang (not MSVC); capture always 2PC at stage 3; authorization stays single-wallet.
- Patterns to follow: lock ordering on striped two-lock paths; shared-nothing partitions; observer structs are copyable and hold no engine pointers.
- Anti-goals: do not chase latency after the three write-ups; do not put Qt in observer.



## File map



### Goal: Performance and observer


| Path                               | Create/Modify | Responsibility                                                                                  | Public surface                                                                                                     | Verified/Provisional             |
| ---------------------------------- | ------------- | ----------------------------------------------------------------------------------------------- | ------------------------------------------------------------------------------------------------------------------ | -------------------------------- |
| `aegis/ledger/ledger.hpp`          | modify        | Stripe count parameter on the constructor; verbs stay the M2 surface but are internally striped | stage 1 engine behaviour remains the default until measured; `reserve` / `capture` / `reverse` / `expire_due` stay | verified                         |
| `aegis/ledger/striped.hpp`         | create        | Stage 2: hash wallet → stripe lock; ordered lock acquire                                        | internal                                                                                                           | provisional                      |
| `aegis/ledger/idempotency.hpp`     | modify        | Stage 2: internal mutex; `find_*` return value copies instead of pointers into the map          | `find_reserve` / `find_hold_id` / `find_capture` / `find_reverse` → `std::optional`                                | provisional                      |
| `aegis/authorizer/authorizer.hpp`  | modify        | Stage 2: response cache mutex and value-copy lookup                                             | private                                                                                                            | provisional                      |
| `aegis/issuersim/issuersim.hpp`    | modify        | Stage 2: `calls` becomes atomic so concurrent `handle` calls do not race                        | `IssuerSim::calls`                                                                                                 | provisional                      |
| `aegis/engine/engine.hpp`          | modify        | `EngineConfig::ledger_stage`; global mutex held only in stage 1                                 | `LedgerStage::{GlobalMutex, Striped}`                                                                              | provisional                      |
| `fixtures/genesis_stage2.csv`      | create        | Eight funded cardholders plus merchant and system for the shared stage-1/stage-2 bench recipe   | —                                                                                                                  | provisional                      |
| `bench/results/stage1.txt`         | modify        | Re-measured under the shared multi-cardholder recipe so stage 2 compares like for like          | —                                                                                                                  | provisional                      |
| `tests/ledger_lifecycle_test.cpp`  | modify        | Mechanical adaptation to the copy-out idempotency API (pointers → `std::optional`)              | —                                                                                                                  | provisional                      |
| `aegis/ledger/partition.hpp`       | create        | Stage 3: one thread per wallet partition                                                        | internal                                                                                                           | provisional                      |
| `aegis/ledger/twophase.hpp`        | create        | Capture 2PC across Cardholder/Merchant/System partitions                                        | capture coordination used by `Ledger::capture`                                                                     | provisional                      |
| `aegis/metrics/histogram.hpp`      | create        | Latency histogram, throughput                                                                   | `record`, percentile query                                                                                         | provisional                      |
| `aegis/observer/snapshot.hpp`      | create        | Copyable `MetricsSnapshot`                                                                      | fields listed in T2                                                                                                | provisional                      |
| `aegis/observer/ring.hpp`          | create        | Bounded lossy transaction ring                                                                  | `push`, `drain`                                                                                                    | provisional                      |
| `aegis/observer/commands.hpp`      | create        | start/stop, inject slow issuer / timeouts / drop                                                | `ObserverCommands`                                                                                                 | provisional                      |
| `aegis/observer/observer.hpp`      | create        | Facade used by GUIs and later `aegisd` websocket                                                | `snapshot()`, `drain()`, commands                                                                                  | provisional                      |
| `apps/aegis-load/main.cpp`         | modify        | Configurable rate, duration, terminal count                                                     | CLI flags                                                                                                          | provisional (file arrives in M3) |
| `apps/aegisd/main.cpp`             | modify        | Benchmark mode: print p50/p99/p99.9 + throughput                                                | `--bench`                                                                                                          | provisional (file arrives in M3) |
| `bench/results/.gitkeep`           | create        | Committed result files                                                                          | —                                                                                                                  | provisional                      |
| `bench/RESULTS.md`                 | create        | Three runs + why each change helped                                                             | markdown                                                                                                           | provisional                      |
| `tests/ledger_stage2_test.cpp`     | create        | Parallel unrelated wallets; ordered locks if two stripes are taken                              | —                                                                                                                  | provisional                      |
| `tests/ledger_stage3_2pc_test.cpp` | create        | Capture 2PC vs shadow; failure rolls back                                                       | —                                                                                                                  | provisional                      |
| `tests/observer_test.cpp`          | create        | Snapshot copy; ring overwrite when full; commands hold no engine pointers                       | —                                                                                                                  | provisional                      |
| `.github/workflows/ci.yml`         | modify        | TSan job, concurrent smoke                                                                      | —                                                                                                                  | verified                         |




### Blast radius


| Path                     | Why sensitive                          | Behavior that must stay intact                                                                                                  |
| ------------------------ | -------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------- |
| `aegis/ledger/*` stage 3 | Money + 2PC; easy to desync the shadow | M2 capture amounts (`split_capture`) and “no reverse after capture”. Failed 2PC leaves balances unchanged. Shadow still matches |
| `aegis/observer/*`       | M5 GUI contract                        | Snapshot and command fields below. No Qt types. No pointers into the engine                                                     |
| Engine thread topology   | False sharing / races                  | TSan smoke is green before M4 is called done. Authorization remains single-wallet                                               |




## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition).
2. Per subtask: **Plan mode** + **planning-subtasks** → **CreatePlan** type-2 plan when **Plan mode** is `high` (or `medium` with a real remaining unknown).
3. **Agent mode**: **test-driven-development** when `TDD suitable: yes` (or the TDD slice of `partial`); follow **Verification** when `no`.
4. Equivalent internal changes: record and continue. Material changes: pause and confirm, then log a **Plan changelog** row.

Changing a snapshot or command field is material: pause and update this plan and M5 together.

## Subtasks

Dependency notation: `Blocked by: T1` means start after T1 is done.

### T1 — Histogram + `aegisd --bench` + `aegis-load` rate

- [x] **Do:** Record latency/throughput; configurable load; write stage-1 numbers to `bench/`.

- **Consumes:** M3 `Engine`, `aegisd`, `aegis-load`, `snapshot_count`
- **Produces:** `histogram::record` and a percentile query for p50/p99/p99.9; `aegis-load` flags for rate, duration, terminal count; `aegisd --bench` prints those percentiles and throughput; one committed stage-1 file under `bench/results/`
- **Acceptance:** Feeding known latencies returns the expected p50/p99/p99.9. A stage-1 run writes a result file. The write-up section can be a stub until T3/T4, but the numbers are real.
- **Blocked by:** —
- **Plan mode:** medium — named unknown: exact stored samples versus fixed buckets, as long as p50/p99/p99.9 match the acceptance samples.
- **TDD suitable:** partial
- **TDD suitable reason:** histogram math is TDD; CLI/benchmark harness is measurement/checklist.
- **Verification:** Histogram unit tests; one committed stage-1 result file.



### T2 — Observer API (Qt-free)

- [x] **Do:** `MetricsSnapshot`, lossy ring, start/stop + three fault injects; no pointers into the engine.

- **Consumes:** T1 histogram percentiles; M3 running engine
- **Produces:** `MetricsSnapshot` with throughput, approval rate, p50/p99/p99.9, queue depth, ledger health (invariant flag, live holds, WAL size, account count), worker and partition counts, uptime, running state. `push`/`drain` on a bounded ring that overwrites the oldest entry when full. Commands: start, stop, set worker/partition counts where safe, inject slow issuer, force timeouts, drop connections.
- **Acceptance:** A copied snapshot does not alias engine memory. After more `push`es than capacity, `drain` returns only the newest entries. Command objects contain no engine pointers. Headers compile in a translation unit that does not include Qt.
- **Compatibility:** M5 reads only this facade. Adding or renaming a field is a material change.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `observer_test`; `aegis` headers compile without Qt.



### T3 — Ledger stage 2 striped locks

- [x] **Do:** Hash wallets to N locks; ordered acquire; measure; write analysis vs stage 1.

- **Consumes:** T1 bench harness; M2 `Ledger` verbs and shadow model
- **Produces:** stage-2 lock map (wallet hash → stripe); ordered acquire when two stripes are taken; thread-safe idempotency store and authorizer response cache (value copies, no pointers into shared maps); atomic issuer call counter; `EngineConfig::ledger_stage` with `aegisd --ledger-stage`; `aegis-load --pans` and `fixtures/genesis_stage2.csv` for a multi-cardholder recipe; stage-1 and stage-2 result files plus the `bench/RESULTS.md` stage-2 section
- **Acceptance:** Unrelated wallets post in parallel under the test. Two-stripe capture or transfer always locks in a fixed order. Shadow balances match. Concurrent `handle()` calls on distinct keys return the expected codes with an exact issuer call count. With `ledger_stage = Striped` the engine approves concurrent authorizations on distinct cardholders through the same verbs. Both stages are measured under one multi-cardholder recipe so the comparison is like for like. The write-up states what changed versus stage 1.
- **Compatibility:** `Ledger::reserve` is still one wallet. External verb signatures from M2 stay.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `ledger_stage2_test`; shadow agrees; `bench/RESULTS.md` stage 2 section.



### T4 — Ledger stage 3 partitions + capture 2PC

- [x] **Do:** One writer thread per wallet partition; auth intra-partition; capture 2PC across Cardholder, Merchant, and System; shadow agrees under load.

- **Consumes:** T3 measured stage 2; M2 `split_capture` and shadow model
- **Produces:** partition worker threads; `capture` prepared on each partition and committed or rolled back together
- **Acceptance:** Authorization of one cardholder does not take another partition’s lock. A successful capture still pays `split_capture` amounts. If any participant fails, all three balances match the pre-capture shadow. Load-test shadow agrees, including captures.
- **Compatibility:** No partial capture. No reverse after a committed capture. Stage 1 and 2 money outcomes stay the same.
- **Blocked by:** T3
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `ledger_stage3_2pc_test`; load vs shadow; stage 3 bench + write-up.



### T5 — ThreadSanitizer CI smoke

- [x] **Do:** Linux Clang TSan job on a concurrent engine smoke (auth + capture mix).

- **Consumes:** T4 stage-3 engine
- **Produces:** a TSan job in `.github/workflows/ci.yml`; `bench/RESULTS.md` complete for three runs
- **Acceptance:** The TSan job exits 0 on the auth+capture smoke. `RESULTS.md` has stage 1, 2, and 3 numbers and a short reason each change helped.
- **Blocked by:** T4
- **Plan mode:** skip
- **TDD suitable:** no
- **TDD suitable reason:** CI config; races are found by the TSan run, not gtest-first.
- **Verification:** TSan job green; `RESULTS.md` complete (three runs).



## TDD note (Agent mode)

Per subtask, obey `TDD suitable`: `yes` means strict **test-driven-development** (red/green/refactor); `partial` applies it only to the testable slice; `no` means do not force test-first — still satisfy **Verification**. A behavior-preserving refactor with a covering suite is `no` (stay-green), not a new RED/GREEN pair. Type-2 planning → **planning-subtasks** skill. UI work is often `partial`: TDD the behavior core, verify polish and motion manually.

T2–T4 are `yes`. T1 is `partial` (histogram math only). T5 is `no`.

## Plan changelog


| Date       | Change                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                         | Kind       |
| ---------- | -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | ---------- |
| 2026-08-15 | Initial plan                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                   | —          |
| 2026-10-01 | Reshaped to the updated writing-plans scaffold. Snapshot fields copied from the spec into T2 so M5 has a contract. T1 is `medium` (histogram storage). T5 is `skip`. T2–T4 stay `high`. TDD tags unchanged                                                                                                                                                                                                                                                                                                                                                                                                     | equivalent |
| 2026-10-23 | T3 scope extended after research (user-agreed before implementation). Stage 1 is the engine's global mutex around the whole `Authorizer::handle`, which also protects the idempotency store, the response cache, and `IssuerSim::calls`; ledger-only striping therefore cannot produce a bench delta. T3 now covers that whole path plus an engine stage switch, and adds `idempotency.hpp`, `authorizer.hpp`, `issuersim.hpp`, `engine.hpp`, `fixtures/genesis_stage2.csv`, `bench/results/stage1.txt` (re-measure), and `tests/ledger_lifecycle_test.cpp` (copy-out API) to the file map. TDD tags unchanged | material   |
| 2026-10-23 | T3 implemented. Bench recipe changed from the planned 200 req/s to 1000 req/s with 32 terminals: at 200 req/s both stages plateau at ~150 req/s because `aegis-load` opens one connection per request, so the client hides any ledger difference. At the hotter rate latency separates clearly (p50 −42 %, p99 −45 %) while throughput stays at the client ceiling; `bench/RESULTS.md` states this. `EngineConfig::ledger_stage` now defaults to `Striped`, with `--ledger-stage 1` kept for reproduction                                                                                                      | equivalent |
| 2026-11-09 | T4 implemented. Stage-3 hold ids encode the owning partition (`partition << 32 \| local`) so capture/reverse route without a directory; `LedgerMode::Partitioned` + `--partitions`/`ledger_partitions` (default 8) select the stage; stage-3 WAL appends one `C` record after all prepares and before any commit (fixes the claim-then-validate wart for stage 3 only, stages 1/2 untouched); bench files gain `partitions=`. All three stages were re-measured in one session because the environment changed; stage 3 trades a small p50 increase for the best p99 and removes all shared locks. `aegis-load`/authorizer capture dispatch remains out of scope (auth-only network mix) | equivalent |
| 2026-11-10 | T5 implemented. `linux-clang-tsan` preset and CI job instrument with `-fsanitize=thread` and run `Stage3Test.ConcurrentAuthAndCaptureSmoke` (concurrent reserve plus cross-partition capture; capture is not on the ISO path). `bench/RESULTS.md` already held the three stage write-ups, so it was left as written. The job can only go green on Linux CI; MSVC has no TSan | equivalent |
| 2026-11-12 | T4 capture isolation. A prepared partition defers ordinary commands until its capture commits or rolls back, and participants prepare in ascending partition order so overlapping captures cannot deadlock. Capture transaction ids are `CaptureTxnId`. Hold release postings share `apply_hold_release`. The TSan job also runs the stage-3 engine authorization smoke and the capture-isolation regressions. ISO capture dispatch and the buffered-WAL contract are unchanged | equivalent |


