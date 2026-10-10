# Implementation plan: Aegis M5 — Dual Consoles

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Goal scope:** Widgets teaching UI and a Vue showcase on the frozen M4 observer API
**Depends on:** [M4 Performance Arc](2026-08-15-aegis-m4-performance-plan.md) (frozen observer)
**Next:** [M6 Settlement](2026-08-15-aegis-m6-settlement-plan.md) (stretch)

## Summary

**M5a:** complete `aegis-console` (Qt Widgets): controls, KPIs, live table (`QAbstractTableModel`), two charts, fault injection; pull snapshots ~30 Hz; no engine pointers. Functional, not pretty.

**M5b:** Vue 3 + Vite + shadcn-vue showcase (KPIs, stream, charts, fault buttons) in `aegis-web` via WebChannel **or** Chrome against `aegisd` local WebSocket if WebEngine is blocked. Package `web/dist` for production.

Out of scope: 1:1 Widgets parity in Vue; settlement views; polishing Widgets.

## Discovery notes

- Reuse: M4 `observer` only (`snapshot`, `drain`, commands). Do not import ledger types in either UI.
- Constraints: `aegis` stays Qt-free; WebEngine only on the `aegis-web` target; npm is not invoked on every C++ rebuild.
- Patterns to follow: GUI pulls; lossy ring for the table; commands for start/stop/faults.
- Anti-goals: no Electron; do not start Vue before M5a can drive the observer.

## File map

### Goal: M5a Widgets

| Path | Create/Modify | Responsibility | Public surface | Verified/Provisional |
|------|---------------|----------------|----------------|----------------------|
| `apps/aegis-console/main.cpp` | modify | Host window, engine threads | — | provisional (bare window arrives in M3) |
| `apps/aegis-console/main_window.hpp` | create | Four-band layout | `MainWindow` | provisional |
| `apps/aegis-console/metrics_bar.hpp` | create | KPI labels from snapshot | formatting helpers if extracted | provisional |
| `apps/aegis-console/stream_model.hpp` | create | `QAbstractTableModel` over drained ring | model | provisional |
| `apps/aegis-console/charts.hpp` | create | Throughput + latency percentile charts | Qt Charts or a small custom paint | provisional |
| `apps/aegis-console/fault_bar.hpp` | create | Inject buttons → observer commands | — | provisional |
| `vcpkg.json` | modify | Qt6 Widgets (+ Charts if used); still no WebEngine on this target | — | verified |

### Goal: M5b Vue desktop

| Path | Create/Modify | Responsibility | Public surface | Verified/Provisional |
|------|---------------|----------------|----------------|----------------------|
| `web/package.json` | create | Vue 3, Vite, TS, Tailwind, shadcn-vue | npm scripts | provisional |
| `web/src/App.vue` | create | Showcase layout | — | provisional |
| `web/src/bridge.ts` | create | WebChannel **or** WebSocket client to the same snapshot/commands | `getSnapshot`, `drain`, `command` | provisional |
| `apps/aegis-web/main.cpp` | create | `QWebEngineView` + WebChannel adapter **outside** `aegis` | — | provisional |
| `apps/aegis-web/CMakeLists.txt` | create | Optional WebEngine target | `aegis-web` | provisional |
| `apps/aegisd/websocket.cpp` | create | Local WebSocket fallback for Vite/Chrome | optional flag | provisional |
| `CMakeLists.txt` | modify | Copy `web/dist` next to `aegis-web` when present | — | verified |

### Blast radius

| Path | Why sensitive | Behavior that must stay intact |
|------|----------------|--------------------------------|
| `aegis/observer/*` | Shared contract | Do not add a field here. If a field is missing, that is a material change to the M4 plan first |
| `apps/aegis-web` + vcpkg WebEngine | Can stall for weeks on Windows | Timebox. Browser + `aegisd` WebSocket is a successful M5b. `aegis-console` and `aegisd` build without WebEngine |
| `apps/aegis-console/stream_model.hpp` | Fast table; easy to block the GUI thread | Drain on the timer. Never lock the engine on the GUI thread |

## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition).
2. Per subtask: **Plan mode** + **planning-subtasks** → **CreatePlan** type-2 plan when **Plan mode** is `high` (or `medium` with a real remaining unknown).
3. **Agent mode**: **test-driven-development** when **`TDD suitable: yes`** (or the TDD slice of **`partial`**); follow **Verification** when **`no`**.
4. Equivalent internal changes: record and continue. Material changes: pause and confirm, then log a **Plan changelog** row.

Finish T1–T2 (M5a) before T3 (M5b). If the WebEngine path becomes the fallback, log that as equivalent and keep the browser path as the acceptance case already written in T4.

## Subtasks

Dependency notation: `Blocked by: T1` means start after T1 is done.

### T1 — Widgets shell: start/stop + KPI bar

- [ ] **Do:** Main window bands; QTimer snapshot; start/stop commands; KPI labels (tps, approve %, p50/p99/p99.9, queue depth).
- **Consumes:** M4 `MetricsSnapshot` and start/stop commands
- **Produces:** `MainWindow` four-band shell; a timer ~30 Hz that calls `snapshot()`; labels bound to throughput, approval rate, p50, p99, p99.9, queue depth
- **Acceptance:** Formatting helpers, if extracted, map a fixture snapshot to those strings. Start/stop calls the observer commands and does not touch the ledger. The window shows the labels against `aegis-load`.
- **Compatibility:** Pull only. No engine pointer is stored in a widget.
- **Blocked by:** —
- **Plan mode:** medium — named unknown: which `MetricsSnapshot` members bind to which labels, confirmed against the M4 header once it exists.
- **TDD suitable:** partial
- **TDD suitable reason:** snapshot mapping can be unit-tested; layout is visual.
- **Verification:** Headless tests for formatting helpers if extracted; manual window against `aegis-load`.

### T2 — Stream model + charts + fault bar

- [ ] **Do:** `QAbstractTableModel` from ring drain; two charts with axis units; inject slow issuer / timeouts / drop connections.
- **Consumes:** T1 window; M4 `drain` and fault commands
- **Produces:** table model whose rows are drained ring entries; throughput chart (last 60 seconds) and latency-percentile chart, both with axis units; buttons for slow issuer, force timeouts, and drop connections
- **Acceptance:** A fake ring drain fills model rows without calling into the engine. Charts show units. Injected timeouts become reversals on a manual run. Expire-now is shown only if M4 already exposes that command; do not add it here.
- **Compatibility:** The model does not lock the engine. Fault buttons call observer commands only.
- **Blocked by:** T1
- **Plan mode:** medium — named unknown: Qt Charts versus a small custom paint for the two charts. Drain stays on the GUI timer either way.
- **TDD suitable:** partial
- **TDD suitable reason:** model row mapping is TDD; charts and layout are manual.
- **Verification:** Model unit tests with a fake ring; manual fault demo (timeouts become reversals).

### T3 — Vue app + observer client (browser first)

- [ ] **Do:** Vite/Vue/shadcn-vue: KPIs, table, charts, fault buttons talking to `aegisd` WebSocket. No WebEngine required yet.
- **Consumes:** T2 proof that the observer drives a UI; M4 snapshot and commands
- **Produces:** `web/` app; `bridge.ts` `getSnapshot`, `drain`, `command`; `aegisd` local WebSocket that speaks those three operations
- **Acceptance:** Vitest covers bridge encode/decode of a snapshot and the three fault commands. Chrome plus `aegisd` shows KPIs, a stream, charts, and the fault buttons.
- **Compatibility:** The WebSocket payload is a projection of the M4 observer. It does not add ledger fields.
- **Blocked by:** T2
- **Plan mode:** high
- **TDD suitable:** partial
- **TDD suitable reason:** `bridge.ts` mapping is TDD/vitest; UI polish is manual.
- **Verification:** Vitest on the bridge; Chrome + `aegisd` smoke.

### T4 — `aegis-web` WebEngine host + dist packaging

- [ ] **Do:** Thin Qt host, WebChannel adapter in the **app** not in `aegis`; CMake copies `web/dist`. If WebEngine fails the timebox, the browser fallback already in T3 is M5b done.
- **Consumes:** T3 `web/dist` and `bridge.ts`
- **Produces:** target `aegis-web` (`QWebEngineView` + WebChannel) **or** a README note that the WebSocket fallback is the shipped path; CMake copies `web/dist` beside the binary when the target exists
- **Acceptance:** Packaged UI loads with no Vite dev server, in `aegis-web` or in Chrome against `aegisd`. `aegis-console` and `aegisd` still configure without WebEngine installed.
- **Compatibility:** WebEngine stays off `aegis`, `aegis-console`, and the default `aegisd` build.
- **Blocked by:** T3
- **Plan mode:** high
- **TDD suitable:** no
- **TDD suitable reason:** packaging/WebEngine wiring; no practical unit path. Verification is smoke plus the fallback checklist.
- **Verification:** `aegis-web` loads the packaged UI **or** the README marks the WebSocket fallback complete; `aegis-console` / `aegisd` still build without WebEngine.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**: **`yes`** means strict **test-driven-development** (red/green/refactor); **`partial`** applies it only to the testable slice; **`no`** means do not force test-first — still satisfy **Verification**. A behavior-preserving refactor with a covering suite is **`no`** (stay-green), not a new RED/GREEN pair. Type-2 planning → **planning-subtasks** skill. UI work is often **`partial`**: TDD the behavior core, verify polish and motion manually.

T1–T3 are **`partial`**: TDD the mapping helpers and `bridge.ts`; check layout by hand. T4 is **`no`**.

## Plan changelog

| Date | Change | Kind |
|------|--------|------|
| 2026-08-15 | Initial plan | — |
| 2026-10-01 | Reshaped to the updated writing-plans scaffold. T1 and T2 are `medium` with a named unknown. T3 and T4 stay `high` (WebSocket shape and WebEngine timebox). TDD tags unchanged | equivalent |
