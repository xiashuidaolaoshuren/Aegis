# Implementation plan: Aegis M5 — Dual Consoles

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Subsystem scope:** Widgets teaching UI + Vue showcase (WebEngine or browser fallback)
**Depends on:** [M4 Performance Arc](2026-08-15-aegis-m4-performance-plan.md) (frozen observer)
**Next:** [M6 Settlement](2026-08-15-aegis-m6-settlement-plan.md) (stretch)

## Summary

**M5a:** complete `aegis-console` (Qt Widgets): controls, KPIs, live table (`QAbstractTableModel`), two charts, fault injection; pull snapshots ~30 Hz; no engine pointers. Functional, not pretty.

**M5b:** Vue 3 + Vite + shadcn-vue showcase (KPIs, stream, charts, fault buttons) in `aegis-web` via WebChannel **or** Chrome against `aegisd` local WebSocket if WebEngine is blocked. Package `web/dist` for production.

Out of scope: 1:1 Widgets parity in Vue; settlement views; polishing Widgets.

## Discovery notes

- Reuse: M4 `observer` only. Do not import ledger types in either UI.
- Constraints: `aegis` stays Qt-free; WebEngine only on `aegis-web` target; npm not invoked on every C++ rebuild.
- Patterns: GUI pulls; lossy ring for table; commands for start/stop/faults.
- Anti-goals: no Electron; do not start Vue before M5a can drive observer.

## File map

### Subsystem: M5a Widgets

| Path | Create/Modify | Responsibility | Public surface |
|------|----------------|----------------|----------------|
| `apps/aegis-console/main.cpp` | modify | Host window, engine threads | — |
| `apps/aegis-console/main_window.hpp` | create | Four-band layout | `MainWindow` |
| `apps/aegis-console/metrics_bar.hpp` | create | KPI labels from snapshot | — |
| `apps/aegis-console/stream_model.hpp` | create | `QAbstractTableModel` over drained ring | model |
| `apps/aegis-console/charts.hpp` | create | Throughput + latency percentile charts (Qt Charts or custom) | — |
| `apps/aegis-console/fault_bar.hpp` | create | Inject buttons → observer commands | — |
| `vcpkg.json` | modify | Qt6 Widgets (+ Charts if used); still no WebEngine | — |

### Subsystem: M5b Vue desktop

| Path | Create/Modify | Responsibility | Public surface |
|------|----------------|----------------|----------------|
| `web/package.json` | create | Vue 3, Vite, TS, Tailwind, shadcn-vue | npm scripts |
| `web/src/App.vue` | create | Showcase layout | — |
| `web/src/bridge.ts` | create | WebChannel **or** WebSocket client to same snapshot/commands | `getSnapshot`, `drain`, `command` |
| `apps/aegis-web/main.cpp` | create | `QWebEngineView` + WebChannel adapter **outside** `aegis` | — |
| `apps/aegis-web/CMakeLists.txt` | create | Optional WebEngine target | `aegis-web` |
| `apps/aegisd/websocket.cpp` (or similar) | create | Local WS fallback for Vite/Chrome | optional flag |
| `CMakeLists.txt` | modify | Copy `web/dist` next to `aegis-web` when present | — |

### Blast radius

| Path | Why sensitive | Plan mode (before implementation) |
|------|----------------|-----------------------------------|
| `aegis/observer/*` | Shared contract; do not “just add a field” without both UIs | high — if a field is missing, extend observer in a dedicated subtask |
| `apps/aegis-web` + vcpkg WebEngine | Can stall weeks on Windows | high — timebox; fallback WS is success for M5b |
| `apps/aegis-console/stream_model.hpp` | Fast table; easy to block GUI thread | medium — drain on timer, never lock engine |

## Workflow (for implementers)

1. Finish M5a before M5b.
2. Type-2 for observer extensions and WebEngine/WebChannel.
3. TDD on models/commands; manual for layout.
4. Changelog if fallback becomes the primary path.

## Subtasks

### T1 — Widgets shell: start/stop + KPI bar

- [ ] **Do:** Main window bands; QTimer snapshot; start/stop commands; KPI labels (tps, approve %, p50/p99/p99.9, queue depth).
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** partial
- **TDD suitable reason:** snapshot mapping can be unit-tested; layout is visual.
- **Verification:** Headless tests for formatting helpers if extracted; manual window against `aegis-load`.

### T2 — Stream model + charts + fault bar

- [ ] **Do:** `QAbstractTableModel` from ring drain; two charts with axis units; inject slow issuer / timeouts / drop connections; expire-now if observer already exposes it.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** partial
- **TDD suitable reason:** model row mapping TDD; charts/layout manual.
- **Verification:** Model unit tests with fake ring; manual fault demo (timeouts become reversals).

### T3 — Vue app + observer client (browser first)

- [ ] **Do:** Vite/Vue/shadcn-vue: KPIs, table, charts, fault buttons talking to `aegisd` WebSocket. No WebEngine required yet.
- **Blocked by:** T2 (observer proven from Widgets)
- **Plan mode:** high
- **TDD suitable:** partial
- **TDD suitable reason:** `bridge.ts` mapping TDD/vitest; UI polish manual.
- **Verification:** Vitest on bridge; Chrome + `aegisd` smoke.

### T4 — `aegis-web` WebEngine host + dist packaging

- [ ] **Do:** Thin Qt host, WebChannel adapter in **app** not `aegis`; CMake copies `web/dist`. If WebEngine fails timebox, document browser fallback as M5b done.
- **Blocked by:** T3
- **Plan mode:** high
- **TDD suitable:** no
- **TDD suitable reason:** packaging/WebEngine wiring; no practical unit path. Verification is smoke + fallback checklist.
- **Verification:** `aegis-web` loads packaged UI **or** README marks WS fallback complete; `aegis-console`/`aegisd` still build without WebEngine.

## TDD note (Agent mode)

M5 is mostly **`partial`** / T4 **`no`**. Extract pure mapping functions so TDD has a target.

## Plan changelog

| Date | Change |
|------|--------|
| 2026-08-15 | Initial plan |
