# Aegis — Payment Authorization Switch — Design

**Date:** 2026-08-15
**Status:** Approved for planning (domain model locked 2026-08-15; see `CONTEXT.md`)

**Aegis** is a simulated card-authorization switch: ISO 8583 over TCP, a durable
double-entry ledger, and two desktop operations consoles.

## Context

This is a learning project. The goal is to become competent in modern C++ by
building something substantial, with these learning targets:

1. Multi-threaded programming
2. Desktop application development in C++ (Qt Widgets)
3. Advanced C++ technique generally
4. Modern frontend as a desktop UI (Vue hosted in Qt WebEngine)
5. A fintech / payments domain, if it can be arranged naturally

The author is new to C++ but is an experienced programmer in another language,
so the learning curve runs through memory ownership, the build system, and
concurrency primitives rather than programming fundamentals. Budget is roughly
10–15 hours per week over two to three months. The intended outcome is a
portfolio piece for applications to payments companies.

Aegis is the component that sits between point-of-sale terminals and a card
issuer, deciding in milliseconds whether a card payment is approved. It is a
good fit for the learning targets because it is genuinely concurrency-heavy
(many transactions in flight at once) and genuinely correctness-critical (money
must never be created or destroyed), and those two pressures are what force good
C++ habits rather than merely permitting them.

## Goals

- A running system that accepts real ISO 8583 messages over TCP, authorizes
  them against a durable double-entry ledger, and responds under a measurable
  latency budget.
- Two desktop operations consoles on the same engine: a complete Qt 6 Widgets
  teaching UI (`aegis-console`), and a polished Vue 3 showcase hosted in Qt
  WebEngine (`aegis-web`).
- Three successive ledger concurrency designs, each measured, with a written
  explanation of why each change helped.
- A test suite that verifies money-correctness invariants under concurrent
  load, and a crash-recovery test that survives the process being killed.

## Non-goals

Explicitly out of scope, to keep the project finishable:

- No integration with real payment networks or third-party sandbox APIs. The
  card network, the issuing bank, and the terminals are all simulated locally.
- No real cryptography or key management. PIN block and EMV fields are carried
  as opaque bytes; DUKPT key derivation and HSM integration are not implemented.
- No multi-currency conversion. Each account has one currency, and cross-
  currency transactions are rejected.
- No public website, no mobile client, and no product REST API. Vue is an
  embedded desktop UI, not a hosted service. A local WebSocket on `aegisd` is
  allowed only as a WebEngine fallback and a Vite-dev aid.
- No replication, consensus, or distributed operation. Single node only.
- No general-purpose database. Persistence is a purpose-built write-ahead log.
- Not the whole ISO 8583 specification — only the message types and fields
  listed under "Message scope" below.
- No refund or chargeback after capture. Reversal and expiry apply only to a
  live Hold.
- No partial capture. Field 4 on `0200` must equal the original authorization.
- No ISO funding or deposit message. Opening Available comes from a genesis
  fixture at process start.

## Decisions

| Decision | Choice | Reasoning |
| --- | --- | --- |
| Product name | Aegis | Authorization as a shield over funds; short, professional, easy to say |
| Naming | CMake project `Aegis`; library `aegis`; binaries `aegisd`, `aegis-console`, `aegis-web`, `aegis-load` | Consistent terminal and README identity |
| Domain | Payments and ledgers | Career target is payments companies |
| Realism | Self-contained, but a real protocol | Authenticity without API plumbing eating the budget |
| C++ GUI | Qt 6 Widgets | Teaches real desktop C++: model/view, GUI thread, queued signals |
| Frontend desktop UI | Qt WebEngine hosting Vue 3 + Vite + shadcn-vue | Teaches modern SPA-as-desktop without leaving the Qt/C++ project |
| Vue UI library | shadcn-vue, not original shadcn/ui | shadcn/ui is React-first; shadcn-vue is the Vue equivalent |
| GUI split | Widgets = complete teaching UI; Vue = polished demo | Two polished consoles will not fit the calendar; visual polish goes to Vue |
| Networking | Hand-rolled sockets, own thread pool | A framework would hide exactly the concurrency being learned |
| Process layout | Four binaries, one library | Load generation must not share cores with the thing it measures; WebEngine must not infect the engine or the Widgets app |
| Lifecycle scope | Authorization, capture, reversal, expiry | Reversals and expiry are where the interesting correctness problems live |
| Ledger concurrency | Build all three stages, measuring each | The progression is the learning; the destination alone teaches little |
| Language standard | C++20 | `jthread`, `stop_token`, concepts, `span` are all directly useful here |

## Architecture

### Binaries

The `aegis` static library contains the entire engine. It has no GUI
dependency, no Qt dependency, and no global mutable state. Four executables
link against it:

- **`aegisd`** — headless runner. Used for benchmarking and for CI, where no
  display is available. May optionally expose the observer API over a local
  WebSocket so the Vue UI can be developed in a normal browser.
- **`aegis-console`** — Qt 6 Widgets operations application. Runs the engine
  in-process on background threads. This is the complete teaching UI: every
  control, including start/stop and fault injection. Functional, not pretty.
- **`aegis-web`** — thin Qt 6 window whose content is `QWebEngineView`.
  Hosts the Vue 3 showcase. Runs the engine in-process the same way
  `aegis-console` does. Links Qt WebEngine; `aegis-console` and `aegisd` must
  not.
- **`aegis-load`** — a standalone load generator simulating a fleet of POS
  terminals, connecting over real TCP.

Keeping the engine free of Qt is the most important boundary in the design. It
makes payment logic unit-testable without a window, keeps benchmark numbers
free of render overhead, and allows the full engine to run in Linux CI while
development happens on Windows. It also enforces the correct relationship:
both consoles are viewers, not the application.

`aegis-web` is isolated as its own target so a WebEngine/vcpkg failure cannot
block engine work or the Widgets console. If WebEngine cannot be made to build
in reasonable time, the same Vue app falls back to a browser against
`aegisd`'s local WebSocket. That fallback still teaches the frontend; the
WebEngine shell can be wrapped later.

### Components in `aegis`

| Component | Responsibility | Runs on |
| --- | --- | --- |
| `iso8583` | Parse and serialise card messages: bitmaps, BCD, variable-length fields | No threads; pure |
| `net` | Winsock wrapper: listener, connection, length-prefixed framing | Acceptor and I/O threads |
| `concurrent` | Thread pool, bounded queue, later a lock-free ring buffer | Primitives used throughout |
| `ledger` | Account balances, double-entry postings, write-ahead log, recovery | Single writer per partition |
| `authorizer` | Validation, risk rules, the approve or decline decision | Worker pool |
| `issuersim` | Fake network: tunable latency, timeouts, injected `05`. No balances. | Worker pool |
| `metrics` | Per-thread counters and a latency histogram | Written by all, read via observer |
| `observer` | Copyable `MetricsSnapshot`, lossy transaction ring, command API (start/stop, inject fault). Qt-free. | Called from GUI threads or a host-side bridge; never holds engine pointers |

### Thread topology

```
aegis-load (separate process): N simulated POS terminals
   |  TCP, ISO 8583 with 2-byte length prefix
   v
acceptor thread (1)      accepts connections, assigns sockets
   v
I/O threads (M)          read bytes, reassemble frames, parse messages
   |  bounded MPMC queue -- when full, decline with response code 96
   v
worker pool (W)          validate, risk rules, call issuer, decide
   |  posting queue, SPSC per worker
   v
ledger writer (1 per partition)   apply postings, append to WAL
   v
WAL and periodic snapshots on disk
   |
observer snapshot  -->  aegis-console GUI thread at 30 Hz
                   -->  aegis-web via Qt WebChannel (same snapshot)
```

`M` and `W` are runtime-tunable so their effect on throughput and latency can
be measured rather than guessed.

Two properties of this topology are deliberate:

**One writer per slice of the ledger.** Money is the one thing that must never
be wrong, so the design makes concurrent corruption structurally impossible
rather than depending on locking being correct. Real ledger systems converge on
the same trade.

**Both GUIs pull; they are never pushed to.** At full load the engine produces
far more events per second than a screen can render. Each UI samples a
snapshot on a timer, so the engine never blocks on either UI and no unbounded
event backlog can form. Neither UI talks to the ledger directly.

## Domain model

Ubiquitous language lives in [`CONTEXT.md`](../../../CONTEXT.md). Architectural
money decisions: [ADR 0001](../../adr/0001-closed-loop-ledger.md),
[ADR 0002](../../adr/0002-capture-is-cross-wallet.md).

Aegis is **closed-loop**: it is the only ledger. `issuersim` is a fake network
(latency, timeouts, injected `05`), not a second balance book. Response `51`
comes from Cardholder Available.

Money is owned by **Wallets** (`AccountId`): Cardholder (keyed by PAN), Merchant
(keyed by MerchantId), System (Interchange). **Buckets** on those wallets are
the posting lines: Cardholder Available and Holds, Merchant Payable, System
Interchange. `AccountId` never means a single bucket.

### Money

Amounts are integer minor units inside a `Money` type carrying a `Currency`.
Binary floating point cannot represent 0.10 exactly, so a payment system built
on `double` is a payment system that quietly loses money.

Currency arrives at runtime in field 49, so the currency check is a runtime one:
arithmetic between mismatched currencies returns an error rather than silently
coercing, and there is no implicit conversion to or from a raw integer. The
compile-time guarantees live in the strong-type layer below — `Money` cannot be
added to an `int`, and identifiers cannot be interchanged. Screening rejects an
authorization whose field 49 does not match the Cardholder wallet currency.

### Strong types

Every identifier has its own type rather than being an `int` or a
`std::string`: `AccountId` (wallet), `MerchantId`, `TerminalId`, `Stan`, `Rrn`,
`Pan`. All are generated from one small `Tagged<T, Tag>` template. Passing a
terminal ID where a wallet ID belongs must not compile.

### Card number handling

`Pan` stores the full number, but its only formatting path emits a masked form
such as `424242******4242`. The full value is reachable solely through an
explicitly named accessor used at the single place that needs it. Logs, error
messages, and the GUI therefore cannot print a full card number by accident.
This mirrors PCI-DSS requirements for a small amount of code.

### Message scope

Message types implemented:

- `0100` / `0110` — authorization request and response
- `0200` / `0210` — capture of a prior authorization, referencing it via field 90
- `0400` / `0410` — reversal request and response
- `0800` / `0810` — network management, used for keep-alive and sign-on

Using `0200` for capture is a deliberate simplification. Real acquirers capture
through clearing files or `0220` advice messages; modelling it as a request and
response over the same connection keeps the whole lifecycle observable in one
place, which serves the learning goal better than protocol fidelity would here.

Fields implemented: 2 (PAN), 3 (processing code), 4 (amount), 7 (transmission
date and time), 11 (STAN), 12 and 13 (local time and date), 37 (RRN), 38
(authorization ID), 39 (response code), 41 (terminal ID), 42 (merchant ID), 49
(currency code), 52 (PIN block, opaque), 90 (original data elements, required
for capture and reversal).

Response codes used: `00` approved, `05` do not honour, `30` format error, `51`
insufficient funds, `96` system malfunction (returned under backpressure).

### Transaction lifecycle

```
Received -> Validated -> Screened -> Reserved -> Authorized -> Captured -> Settled
   |            |            |            |            |                    (M6)
   v            v            v            v            v
Replayed     Rejected     Declined     Reversed     Reversed
(duplicate)  (resp 30)    (05/51,     (issuer       (0400 while
                           no hold)    timeout/05     hold is live)
                                       or TTL expiry)
```

**Received → Validated.** Format and required fields. Failure: `30`, no Hold.

**Screened.** Hard local rules *before* reserve: currency matches wallet, amount
> 0, optional max-amount. Failure: `05`, no Hold.

**Funds check.** If Available would go negative: `51`, no Hold, `issuersim` is
not called.

**Reserved.** Hold posted (Available → Holds), then wait on `issuersim`.

**Authorized.** Issuer simulator returns `00`. Hold stays until capture,
reversal, or expiry.

**Reversed.** Timeout, injected `05` after reserve, `0400`, or TTL/sweeper /
inject-expire. Hold released (Holds → Available). Not valid after Capture.

**Captured.** Full Hold amount; fee split to Merchant Payable and Interchange.
Always cross-wallet (Stage 3: two-phase on this path).

**Settled.** M6 stretch: discharge Payable. Capture does not pay the merchant
out.

Every state that moves money writes to the ledger. Screening/`51` do not.

### Postings

Cardholder and merchant wallets are liabilities of Aegis, so a debit decreases
them and a credit increases them. Interchange follows the same convention.
Every event produces a balanced set of postings. A 50.00 purchase at a 2.9
percent merchant fee (fee remainder stays with the merchant so the Hold is fully
consumed):

**At reserve (authorization hold)** — debit Cardholder Available 50.00, credit
Cardholder Holds 50.00. Intra-wallet. No money has left the Cardholder; 50.00
became unavailable to spend.

**At capture** — debit Cardholder Holds 50.00, credit Merchant Payable 48.55,
credit System Interchange 1.45. Cross-wallet. Field 4 must equal 50.00; the
split is a fee, not a partial capture.

**On reversal or hold expiry** — invert the reserve: debit Holds, credit
Available, same amount. Intra-wallet. Forbidden after Capture.

### Genesis

Process start loads a fixture of wallets and opening Available (tests: a tiny
set; load runs: a generated population). There is no ISO message that funds a
wallet.

### Idempotency

The idempotency key is TerminalId + STAN + field 7 date (MMDD). A retransmitted
`0100` returns the stored original response rather than reserving again.
Capture and reversal locate the original authorization via field 90. `0200` and
`0400` are themselves idempotent on their own key so a retried capture does not
move money twice. Because ISO 8583 terminals retransmit on timeout as a matter
of course, this is not an edge case — it is normal traffic.

### Hold expiry

Each Hold has a configurable TTL (short in tests and demos). A sweeper expires
due Holds with the same posting as reversal. The console can inject expire-now.

## Invariants

Checked continuously under load by the test harness, not once at the end of a
run:

1. **Debits equal credits, always.** After every posting batch the sum across
   all buckets is exactly zero. Debug builds assert this on each write.
2. **Available never goes negative.** An authorization that would break this is
   declined with `51` before any Hold is posted and before `issuersim` is called.
3. **One authorization, one live Hold.** A reserved or authorized `0100` has
   exactly one live Hold until capture, reversal, or expiry.
4. **A duplicate never moves money twice.** Same idempotency key returns the
   stored response; retried `0200`/`0400` do not post twice.
5. **Replay reproduces state exactly.** Killing the process mid-load and
   restarting must rebuild identical bucket balances from the write-ahead log.

## Concurrency arc

The ledger is built three times. Each version is measured before the next is
started, and the reason it was insufficient is written down.

**Stage 1 — one global mutex.** Every worker locks the same mutex to read a
balance and write a posting. Correct immediately, and slow as soon as workers
are added, because the lock serialises the interesting work. Teaches
`std::mutex`, `lock_guard`, RAII locking, and the shape of a measured
contention curve.

**Stage 2 — striped locks.** Accounts hash to one of N locks so unrelated
transactions proceed in parallel. A transfer touching two accounts now needs
two locks, and acquiring them in inconsistent order deadlocks. The fix is a
total ordering on acquisition. Teaches lock granularity, hash partitioning,
deadlock and lock ordering.

**Stage 3 — wallet partitions, no locks.** Each partition owns a disjoint
slice of wallets and runs on exactly one thread, so within a partition there
is no sharing and nothing to lock. Workers route by wallet. Authorization is
single-partition (one Cardholder). Capture is always cross-partition (Cardholder
+ Merchant + System) and uses two-phase commit — that is the capture path, not
a rare transfer. Teaches shared-nothing design, message passing, lock-free ring
buffers, atomics and memory ordering, and 2PC.

Three recorded benchmark runs plus a written analysis is a stronger artifact
than a fast system with no story.

## Advanced C++ technique map

Each technique is introduced at the point the project creates a reason for it.

| Technique | Introduced while building |
| --- | --- |
| RAII and ownership discipline | The socket and file wrappers |
| Smart pointers, unique ownership | The codec and ledger |
| Strong types via `Tagged<T, Tag>` | `Money`, `AccountId`, `MerchantId`, `Pan`, `Stan` |
| `constexpr` specification tables | The ISO 8583 field definitions |
| `std::span` and `string_view` | Zero-copy parsing over the read buffer |
| `Result<T, E>` error values | The codec and the authorizer |
| Move semantics | Handing message buffers between threads |
| `std::jthread` and `stop_token` | Cooperative shutdown of the thread pool |
| PImpl | Keeping ledger internals out of public headers |
| Concepts | Optional later; Screening in committed scope is hard rules, not a plugin |
| Atomics and memory ordering | Metrics snapshots, then the ring buffer |
| Cache-line padding | Removing false sharing between counters |
| Object pools and allocators | Taking allocation off the hot path |

## Operations consoles

Two desktop UIs share one observer API. They do not share widgets, Vue
components, or Qt modules beyond that API.

### Shared observer API

The `aegis` library exposes:

- `MetricsSnapshot` — a plain copyable struct with no pointers into engine
  memory: throughput, approval rate, latency percentiles, queue depth, ledger
  health (invariant flag, live holds, WAL size, account count), worker and
  partition counts, uptime, running state.
- A bounded, lossy transaction ring the UI drains. Old entries are overwritten
  if the UI falls behind. The on-screen stream need not be complete; the
  write-ahead log is the lossless record and must never drop anything.
- A command API: start, stop, set worker/partition counts where safe, inject
  faults (slow issuer, force timeouts, drop connections).

Neither console imports ledger types. Both pull snapshots on a timer (~30 Hz).
Nothing mutable is shared across the GUI boundary, so there is no lock in the
render path and no way to observe a half-updated number.

Fault injection is on the main screen of both UIs rather than hidden in a menu,
because a demo where everything succeeds proves very little.

### `aegis-console` — Qt Widgets (complete teaching UI)

One window in four bands:

1. **Controls** — start and stop, worker and partition counts, connected
   terminal count, uptime and running state.
2. **Headline numbers** — authorizations per second, approval rate, p50, p99
   and p99.9 latency, request queue depth.
3. **Main area** — the live transaction stream table beside two charts:
   throughput over the last 60 seconds, and latency percentiles over the same
   window. Both charts label their axes with units.
4. **Bottom strip** — ledger health and fault injection controls.

This app is functionally complete. Visual polish is not a goal here. The
transaction table is a `QAbstractTableModel`, not a `QTableWidget`, because
model/view is the C++ desktop pattern worth learning. A `QTimer` on the GUI
thread calls `snapshot()`.

### `aegis-web` — Vue showcase in Qt WebEngine

A thin C++ host: a `QMainWindow` containing a `QWebEngineView`, plus enough
native chrome to be a real window (title, close). The host runs the engine
in-process and bridges the observer API into JavaScript with **Qt WebChannel**
(the adapter lives in `aegis-web`, not in `aegis`).

The page is a Vue 3 + Vite SPA using **shadcn-vue** (not React shadcn/ui). It is
the polished demo: KPIs, live stream, charts, and fault buttons. It does not
need 1:1 parity with every Widgets control on day one. Extra Widgets-only
controls can be copied later if time remains.

**Dev vs ship:**

- Development: the host loads `http://localhost:5173` so Vite hot-reload works.
  Optionally the same SPA can be opened in Chrome against `aegisd`'s local
  WebSocket.
- Production: Vite writes `dist/`; CMake copies it next to the binary (or into
  Qt resources); the host loads `index.html` from disk.

`aegis-web` is the binary that ships Chromium. Expect a large download and a
large artifact. That cost is confined to this target.

### Sequence

Engine and Widgets first. The Vue desktop app starts only after the observer
API exists and `aegis-console` can drive it. Chromium setup must not block
the codec, ledger, or concurrency arc.

## Error handling

**On the hot path, errors are values.** Parsing, authorizing, and posting
return `Result<T, Error>`. A declined transaction is a normal outcome, not an
exception, and throwing at high frequency through a worker pool is both slow
and hard to reason about.

**At startup, exceptions are fine.** Configuration and initialisation failures
have nothing sensible to do but abort.

**The parser never crashes.** A malformed message produces a format-error
response. This single rule is what makes the codec worth fuzzing.

**Backpressure sheds load rather than buffering it.** When the request queue
fills, the switch declines with response code 96 instead of growing memory
without bound.

## Testing strategy

**A shadow model is the centrepiece.** Alongside the real ledger the harness
keeps a deliberately naive, obviously-correct implementation: a map from
account to balance, single-threaded, unoptimised. Both are driven with the same
transaction stream and asserted to agree. This oracle is what makes it safe to
rewrite the ledger three times — if stage 3 disagrees with the shadow model
anywhere, stage 3 is wrong.

Around it:

- **Unit tests** (GoogleTest) for the codec, ledger postings, and risk rules.
- **Property tests** asserting that serialising a parsed message reproduces the
  original bytes, and that postings always sum to zero.
- **Fuzzing** of the parser under Clang with libFuzzer. The only rule is that
  it must never crash.
- **Crash-recovery test** that drives load, kills the process hard, restarts,
  and asserts replayed balances match the shadow model.
- **Race detection** via ThreadSanitizer in Linux CI.
- **Benchmarks** with results committed per milestone.

Note a real platform constraint: MSVC supports AddressSanitizer but not
ThreadSanitizer, and ThreadSanitizer is the tool that finds the data races this
project will generate. The code therefore stays portable enough to build with
Clang or GCC, and CI runs a ThreadSanitizer build on Linux for every push.

## Toolchain and repository layout

- Visual Studio with MSVC (2022 or later) for development; Clang in CI for
  sanitizers. The Windows CMake preset pins the generator installed on this
  machine (`Visual Studio 18 2026` as of M1).
- CMake project name `Aegis`, driven by `CMakePresets.json`, so the same tree
  builds in Visual Studio, VS Code, and CI without three sets of instructions.
- vcpkg in manifest mode (`vcpkg.json`) for Qt 6 Widgets, GoogleTest, and —
  only for the `aegis-web` target — Qt WebEngine. `aegis-console` and `aegisd`
  must build without WebEngine installed.
- Node.js + npm for the Vue app (`web/`): Vue 3, Vite, TypeScript, Tailwind,
  shadcn-vue. The C++ build copies `web/dist` into the `aegis-web` runtime
  directory; it does not run npm as part of every engine rebuild.
- C++20, `clang-format` enforced in CI.

```
aegis/         iso8583, net, concurrent, ledger, authorizer, issuersim, metrics, observer
apps/          aegisd, aegis-console, aegis-web, aegis-load
web/           Vue 3 + Vite + shadcn-vue source (the aegis-web page)
tests/
bench/
docs/
```

## Milestones

Five committed milestones at ten to fifteen hours per week, plus one stretch.
Each milestone ends with something that runs and can be shown. You can stop
after any committed milestone and still have a portfolio-worthy artifact.

Widgets polish is cut before Vue is cut: `aegis-web` is the demo you show;
`aegis-console` is how you learn C++ desktop.

| ID | Name | Weeks | Ships |
| --- | --- | --- | --- |
| M1 | Foundation | 1–3 | Building tree, CI, ISO 8583 codec tested and fuzzed |
| M2 | Ledger | 4–5 | Single-threaded double-entry ledger, holds, WAL, crash recovery |
| M3 | Live Switch | 6–7 | Authorization over TCP + bare Qt Widgets window |
| M4 | Performance Arc | 8 | `aegis-load`, benchmarks, ledger stages 2 and 3, observer API stable |
| M5 | Dual Consoles | 9–13 | Complete Widgets UI, then Vue showcase in `aegis-web` |
| M6 | Settlement | after 13 | Batch reconciliation — stretch, not a commitment |

### M1 — Foundation

**Goal:** A buildable, testable project skeleton and a correct ISO 8583 codec.

**Ships:** `aegis` library (codec only), GoogleTest harness, Windows + Linux CI.
No WebEngine, no GUI, no ledger yet.

**Done when:**

- `aegis` and tests build on Windows (MSVC) and in Linux CI.
- Codec round-trips every message type and field listed under Message scope.
- Property test: serialise(parse(bytes)) reproduces original bytes for a corpus.
- Fuzzer smoke run completes without crash on a seed corpus.
- CMake presets and vcpkg manifest are documented in README stub.

**Learning:** CMake, vcpkg, test harness, templates, `constexpr`, `span`,
`Result`, strong types.

**Depends on:** —

### M2 — Ledger

**Goal:** Money-correct single-threaded ledger with durable recovery.

**Ships:** `ledger` module inside `aegis`, shadow-model test harness, WAL on
disk.

**Done when:**

- Postings always balance; debug build asserts after every write.
- Reserve, capture (full amount, fee split), reversal, and TTL expiry behave
  per the domain section. Genesis fixture loads opening Available.
- Kill-and-replay test: hard-kill mid-batch, restart, bucket balances match
  shadow model exactly.
- Idempotency: duplicate TerminalId+STAN+field 7 date returns stored response
  without a second Hold.

**Learning:** RAII, file I/O, move semantics, PImpl, double-entry invariants.

**Depends on:** M1

### M3 — Live Switch

**Goal:** End-to-end authorization over real TCP with concurrent engine threads.

**Ships:** `aegisd` (or in-process engine runner), `aegis-load` or minimal TCP
client, bare `aegis-console` window with a live counter. Engine still has no
Qt dependency in `aegis`.

**Done when:**

- Client sends `0100` over TCP (2-byte length prefix) and receives `0110`.
- Acceptor, I/O threads, worker pool, and single ledger writer run concurrently.
- Backpressure: full queue returns response code 96, not unbounded memory growth.
- Widgets window shows at least one live metric updating at ~30 Hz.
- Engine unit tests still pass with no display attached.

**Learning:** `std::thread`, `mutex`, `condition_variable`, `jthread`,
hand-rolled sockets, thread-safe queues.

**Depends on:** M2

### M4 — Performance Arc

**Goal:** Measure, optimise, and expose the engine to UIs through a stable
observer API.

**Ships:** `aegis-load` at configurable rate, `aegisd` benchmark mode,
ledger concurrency stages 2 and 3, `observer` module, written benchmark
analysis in `docs/` or `bench/`.

**Done when:**

- Three recorded benchmark runs (stage 1, 2, 3) with p50/p99/p99.9 and
  throughput; short written analysis of why each change helped.
- `MetricsSnapshot`, lossy transaction ring, and command API exist and are
  Qt-free.
- ThreadSanitizer build passes in Linux CI on a concurrent smoke test.
- Shadow model agrees with stage-3 ledger under load, including captures (2PC).

**Learning:** Atomics, memory ordering, lock-free ring buffer, profiling,
contention measurement.

**Depends on:** M3

### M5 — Dual Consoles

**Goal:** Two desktop UIs on the same observer API — complete Widgets teaching
app, then polished Vue showcase.

**Ships:** `aegis-console` (full), `aegis-web` (showcase) or browser fallback.

**M5a — Widgets (weeks 9–10)**

**Done when:**

- KPIs, live transaction table (`QAbstractTableModel`), two charts, start/stop,
  and fault injection (slow issuer, force timeouts, drop connections) all work.
- GUI pulls snapshots; no engine pointer crosses the boundary.

**M5b — Vue desktop (weeks 11–13)**

**Done when:**

- Vue 3 + Vite + shadcn-vue shows KPIs, live stream, charts, and fault buttons.
- Runs in `aegis-web` via WebChannel, **or** in Chrome against `aegisd` if
  WebEngine is blocked on Windows.
- Production build loads packaged `web/dist/` without a dev server.

**Learning:** Qt model/view and cross-thread signals (M5a); Qt WebEngine,
WebChannel, SPA packaging (M5b).

**Depends on:** M4

### M6 — Settlement (stretch)

**Goal:** End-of-day batch reconciliation and merchant payout reports.

**Ships:** Settlement module, optional ops views in existing consoles.

**Done when:**

- Batch ingest matches authorizations to captures; mismatches are classified.
- Merchant payout totals include fees; reports balance to ledger.

**Learning:** Parallel algorithms, memory-mapped I/O.

**Depends on:** M5. Does not block M5 success criteria.

### Week mapping

| Weeks | Milestone | Former phase label |
| --- | --- | --- |
| 1 | M1 toolchain slice | was phase 0 |
| 2–3 | M1 codec slice | was phase 1 |
| 4–5 | M2 | was phase 2 |
| 6–7 | M3 | was phase 3 |
| 8 | M4 | was phase 4 |
| 9–10 | M5a | was phase 5 |
| 11–13 | M5b | was phase 6 |
| after 13 | M6 | was phase 7 |

A bare Qt Widgets window lands in M3 rather than M5 on purpose: running out of
time after M3 still leaves a working graphical application. M5b starts only
after M5a can drive the observer API. If WebEngine setup burns the M5b budget,
ship the Vue app in a browser against `aegisd` and treat the WebEngine window
as leftover work.

## Risks

| Risk | Mitigation |
| --- | --- |
| Qt and vcpkg setup consumes week one and kills momentum | M1 is timeboxed: first week ships only a building skeleton; the engine does not depend on Qt, so a Qt problem never blocks engine work |
| Qt WebEngine fails or eats weeks on Windows | Confined to `aegis-web`. Fallback: same Vue app in the browser against `aegisd`. `aegis-console` is unaffected. |
| Two GUIs duplicate work and blow the calendar | Widgets is functionally complete but not polished. Vue is a showcase subset, not 1:1 parity. M6 is cut first. |
| npm/Vite and CMake fight each other | Vue lives in `web/` and is built separately; CMake only copies `dist/`. Engine rebuilds do not invoke npm. |
| ISO 8583 is a large specification and invites endless scope | The message types and field list above are fixed in this spec; anything else is out of scope |
| Memory-safety bugs from being new to C++ | AddressSanitizer in development builds, RAII discipline enforced by code structure, no raw owning pointers |
| Data races that Windows tooling cannot detect | Portable code plus ThreadSanitizer builds in Linux CI from M1 |
| The GUI slips to the end and never gets built | A minimal Qt Widgets window is an M3 deliverable |
| The optimisation arc becomes an open-ended rabbit hole | M4 ends when three benchmarks are recorded and analysis is written, not when it feels fast |

## Success criteria

Aegis is a success when all of the following hold:

1. A recorded sustained throughput figure with p50, p99 and p99.9 latency,
   reproducible via `aegisd`'s benchmark mode (M4).
2. Three benchmarked ledger stages with a written analysis of each change (M4).
3. The crash-recovery test passing in CI (M2).
4. The parser fuzzer running clean over millions of mutated inputs (M1).
5. `aegis-console` showing live metrics, the transaction stream, and working
   fault injection (M5a).
6. A Vue 3 showcase (KPIs, stream, charts, fault buttons) running either in
   `aegis-web` via Qt WebEngine or, if WebEngine is blocked, in a browser
   against `aegisd` (M5b).
7. A README that explains Aegis well enough for a payments engineer to
   understand the design in five minutes.
