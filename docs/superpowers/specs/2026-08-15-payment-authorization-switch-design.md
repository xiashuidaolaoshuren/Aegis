# Payment Authorization Switch — Design

**Date:** 2026-08-15
**Status:** Approved for planning

## Context

This is a learning project. The goal is to become competent in modern C++ by
building something substantial, with three specific learning targets and one
domain preference:

1. Multi-threaded programming
2. Desktop application development
3. Advanced C++ technique generally
4. A fintech / payments domain, if it can be arranged naturally

The author is new to C++ but is an experienced programmer in another language,
so the learning curve runs through memory ownership, the build system, and
concurrency primitives rather than programming fundamentals. Budget is roughly
10–15 hours per week over two to three months. The intended outcome is a
portfolio piece for applications to payments companies.

The project is a **payment authorization switch**: the component that sits
between point-of-sale terminals and a card issuer, deciding in milliseconds
whether a card payment is approved. It is a good fit for the learning targets
because it is genuinely concurrency-heavy (many transactions in flight at once)
and genuinely correctness-critical (money must never be created or destroyed),
and those two pressures are what force good C++ habits rather than merely
permitting them.

## Goals

- A running system that accepts real ISO 8583 messages over TCP, authorizes
  them against a durable double-entry ledger, and responds under a measurable
  latency budget.
- A Qt 6 desktop operations console showing live throughput, latency
  percentiles, the transaction stream, and ledger health.
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
- No web interface, no mobile client, no REST API.
- No replication, consensus, or distributed operation. Single node only.
- No general-purpose database. Persistence is a purpose-built write-ahead log.
- Not the whole ISO 8583 specification — only the message types and fields
  listed under "Message scope" below.

## Decisions

| Decision | Choice | Reasoning |
| --- | --- | --- |
| Domain | Payments and ledgers | Career target is payments companies |
| Realism | Self-contained, but a real protocol | Authenticity without API plumbing eating the budget |
| GUI framework | Qt 6 Widgets | Best résumé value; its cross-thread signal mechanism teaches thread-safe UI directly |
| Networking | Hand-rolled sockets, own thread pool | A framework would hide exactly the concurrency being learned |
| Process layout | Three binaries, one library | Load generation must not share cores with the thing it measures |
| Lifecycle scope | Authorization, capture, reversal, expiry | Reversals and expiry are where the interesting correctness problems live |
| Ledger concurrency | Build all three stages, measuring each | The progression is the learning; the destination alone teaches little |
| Language standard | C++20 | `jthread`, `stop_token`, concepts, `span` are all directly useful here |

## Architecture

### Binaries

`libswitch` is a static library containing the entire engine. It has no GUI
dependency and no global mutable state. Three executables link against it:

- **`switchd`** — headless runner. Used for benchmarking and for CI, where no
  display is available.
- **`console`** — the Qt 6 operations application. Runs the engine in-process
  on background threads and observes it.
- **`termsim`** — a standalone load generator simulating a fleet of POS
  terminals, connecting over real TCP.

Keeping the engine free of Qt is the most important boundary in the design. It
makes payment logic unit-testable without a window, keeps benchmark numbers
free of render overhead, and allows the full engine to run in Linux CI while
development happens on Windows. It also enforces the correct relationship: the
console is a viewer, not the application.

### Components in `libswitch`

| Component | Responsibility | Runs on |
| --- | --- | --- |
| `iso8583` | Parse and serialise card messages: bitmaps, BCD, variable-length fields | No threads; pure |
| `net` | Winsock wrapper: listener, connection, length-prefixed framing | Acceptor and I/O threads |
| `concurrent` | Thread pool, bounded queue, later a lock-free ring buffer | Primitives used throughout |
| `ledger` | Account balances, double-entry postings, write-ahead log, recovery | Single writer per partition |
| `authorizer` | Validation, risk rules, the approve or decline decision | Worker pool |
| `issuersim` | Simulated issuing bank with tunable latency and decline rate | Worker pool |
| `metrics` | Per-thread counters and a latency histogram | Written by all, read by GUI |

### Thread topology

```
termsim (separate process): N simulated POS terminals
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
metrics snapshot  -->  Qt GUI thread, pulled at 30 Hz
```

`M` and `W` are runtime-tunable so their effect on throughput and latency can
be measured rather than guessed.

Two properties of this topology are deliberate:

**One writer per slice of the ledger.** Money is the one thing that must never
be wrong, so the design makes concurrent corruption structurally impossible
rather than depending on locking being correct. Real ledger systems converge on
the same trade.

**The GUI pulls; it is never pushed to.** At full load the engine produces far
more events per second than a screen can render. The GUI samples a snapshot on
a timer, so the engine never blocks on the UI and no unbounded event backlog
can form.

## Domain model

### Money

Amounts are integer minor units inside a `Money` type carrying a `Currency`.
Binary floating point cannot represent 0.10 exactly, so a payment system built
on `double` is a payment system that quietly loses money.

Currency arrives at runtime in field 49, so the currency check is a runtime one:
arithmetic between mismatched currencies returns an error rather than silently
coercing, and there is no implicit conversion to or from a raw integer. The
compile-time guarantees live in the strong-type layer below — `Money` cannot be
added to an `int`, and identifiers cannot be interchanged.

### Strong types

Every identifier has its own type rather than being an `int` or a
`std::string`: `AccountId`, `TerminalId`, `Stan`, `Rrn`, `Pan`. All are
generated from one small `Tagged<T, Tag>` template. Passing a terminal ID where
an account ID belongs must not compile.

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
for reversals).

Response codes used: `00` approved, `05` do not honour, `30` format error, `51`
insufficient funds, `96` system malfunction (returned under backpressure).

### Transaction lifecycle

```
Received -> Validated -> Screened -> Authorized -> Captured -> Settled
   |            |            |            |                    (phase 6)
   v            v            v            v
Replayed     Rejected     Declined     Reversed
(duplicate)  (resp 30)    (05 / 51)    (timeout or expiry)
```

Every terminal state writes to the ledger, because releasing a hold is as much
a money event as placing one.

### Postings

Cardholder and merchant accounts are liabilities of the switch, so a debit
decreases them and a credit increases them; income accounts follow the same
convention. Every event produces a balanced set of postings. A 50.00 purchase at
a 2.9 percent merchant fee:

**At authorization** — debit `cardholder available` 50.00, credit `cardholder
holds` 50.00. No money has left the system; 50.00 simply became unavailable to
spend, which is precisely what an authorization is.

**At capture** — debit `cardholder holds` 50.00, credit `merchant payable`
48.55, credit `interchange income` 1.45. Debits and credits both total 50.00.

**On reversal or hold expiry** — the authorization pair inverted: debit
`cardholder holds`, credit `cardholder available`, same amount.

### Idempotency

The idempotency key is terminal ID plus STAN plus transmission date. A
retransmitted message returns the stored original response rather than
authorizing again. Because ISO 8583 terminals retransmit on timeout as a matter
of course, this is not an edge case — it is normal traffic.

## Invariants

Checked continuously under load by the test harness, not once at the end of a
run:

1. **Debits equal credits, always.** After every posting batch the sum across
   all accounts is exactly zero. Debug builds assert this on each write.
2. **Available balance never goes negative.** Available equals ledger balance
   minus outstanding holds. An authorization that would break this is declined
   with response code 51.
3. **One authorization, one hold.** Every approved authorization has exactly
   one live hold until it is captured, reversed, or expires.
4. **A duplicate never moves money twice.** Verified by replaying traffic.
5. **Replay reproduces state exactly.** Killing the process mid-load and
   restarting must rebuild identical balances from the write-ahead log.

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

**Stage 3 — account partitions, no locks.** Each partition owns a disjoint
slice of accounts and runs on exactly one thread, so within a partition there
is no sharing and nothing to lock. Workers route transactions to the owning
partition by message. This generalises the single-writer principle rather than
abandoning it. Teaches shared-nothing design, message passing, lock-free ring
buffers, atomics and memory ordering, and why a cross-partition transfer needs
a two-phase protocol.

Three recorded benchmark runs plus a written analysis is a stronger artifact
than a fast system with no story.

## Advanced C++ technique map

Each technique is introduced at the point the project creates a reason for it.

| Technique | Introduced while building |
| --- | --- |
| RAII and ownership discipline | The socket and file wrappers |
| Smart pointers, unique ownership | The codec and ledger |
| Strong types via `Tagged<T, Tag>` | `Money`, `AccountId`, `Pan`, `Stan` |
| `constexpr` specification tables | The ISO 8583 field definitions |
| `std::span` and `string_view` | Zero-copy parsing over the read buffer |
| `Result<T, E>` error values | The codec and the authorizer |
| Move semantics | Handing message buffers between threads |
| `std::jthread` and `stop_token` | Cooperative shutdown of the thread pool |
| PImpl | Keeping ledger internals out of public headers |
| Concepts | The pluggable risk-rule interface |
| Atomics and memory ordering | Metrics snapshots, then the ring buffer |
| Cache-line padding | Removing false sharing between counters |
| Object pools and allocators | Taking allocation off the hot path |

## Operations console

One window in four bands:

1. **Controls** — start and stop, worker and partition counts, connected
   terminal count, uptime and running state.
2. **Headline numbers** — authorizations per second, approval rate, p50, p99
   and p99.9 latency, request queue depth.
3. **Main area** — the live transaction stream table beside two charts:
   throughput over the last 60 seconds, and latency percentiles over the same
   window. Both charts label their axes with units.
4. **Bottom strip** — ledger health (invariant status, live hold count, WAL
   size, account count) and fault injection controls.

Fault injection is on the main screen rather than hidden in a menu, because a
demo where everything succeeds proves very little. Being able to slow the
issuer to 500 ms and watch timeouts become reversals, or fill the queue and
watch the switch shed load with response code 96, is what shows the system was
designed rather than assembled.

### Thread safety at the GUI boundary

The engine exposes a `MetricsSnapshot`: a plain copyable struct with no
pointers into engine memory. A `QTimer` on the GUI thread fires at 30 Hz, calls
`snapshot()`, and receives a value. Nothing mutable is shared across the
boundary, so there is no lock in the render path and no way to observe a
half-updated number.

The transaction stream is different: the engine writes into a bounded ring
buffer and the GUI drains it, overwriting old entries if the GUI falls behind.
This asymmetry is deliberate. The on-screen stream is lossy by design, because
a widget showing the last few hundred transactions need not be complete. The
write-ahead log is the lossless record and must never drop anything.

The transaction table is a `QAbstractTableModel`, not a `QTableWidget`, because
model/view is the pattern worth learning and it handles a fast-scrolling table
without copying every cell into a widget.

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
- **Benchmarks** with results committed per phase.

Note a real platform constraint: MSVC supports AddressSanitizer but not
ThreadSanitizer, and ThreadSanitizer is the tool that finds the data races this
project will generate. The code therefore stays portable enough to build with
Clang or GCC, and CI runs a ThreadSanitizer build on Linux for every push.

## Toolchain and repository layout

- Visual Studio 2022 with MSVC for development; Clang in CI for sanitizers.
- CMake driven by `CMakePresets.json`, so the same tree builds in Visual
  Studio, VS Code, and CI without three sets of instructions.
- vcpkg in manifest mode (`vcpkg.json`) for Qt 6 and GoogleTest, so
  dependencies are declared in the repository rather than installed globally.
- C++20, `clang-format` enforced in CI.

```
libswitch/     iso8583, net, concurrent, ledger, authorizer, issuersim, metrics
apps/          switchd, console, termsim
tests/
bench/
docs/
```

## Build plan

Thirteen weeks at ten to fifteen hours, ordered so every phase ends with
something that runs. Phase 6 is a stretch goal, not a commitment.

| Phase | Weeks | What ships | New C++ ground |
| --- | --- | --- | --- |
| 0 · Toolchain | 1 | Empty project building on Windows and in Linux CI, tests wired up | CMake, vcpkg, test harness |
| 1 · Codec | 2–3 | ISO 8583 parse and serialise, round-trip tested and fuzzed | Templates, `constexpr`, `span`, `Result`, strong types |
| 2 · Ledger | 4–5 | Single-threaded double-entry ledger, holds, WAL, crash recovery | RAII, file I/O, move semantics, PImpl |
| 3 · Concurrent engine | 6–7 | Authorization end to end over TCP, plus a bare Qt window | Threads, mutex, condition variable, `jthread` |
| 4 · Measure and optimise | 8 | `termsim`, benchmark mode, ledger stages 2 and 3 with results | Atomics, memory ordering, lock-free queue, profiling |
| 5 · Console | 9–11 | The full operations console with charts and fault injection | Qt model/view, cross-thread signals |
| 6 · Settlement (stretch) | 12–13 | Batch reconciliation and merchant payout reports | Parallel algorithms, memory-mapped I/O |

A bare Qt window lands in phase 3 rather than phase 5 on purpose: running out
of time then still leaves a working graphical application rather than a
headless engine and a plan.

## Risks

| Risk | Mitigation |
| --- | --- |
| Qt and vcpkg setup consumes week one and kills momentum | Phase 0 is timeboxed to one week and ships only a building skeleton; the engine does not depend on Qt, so a Qt problem never blocks engine work |
| ISO 8583 is a large specification and invites endless scope | The message types and field list above are fixed in this spec; anything else is out of scope |
| Memory-safety bugs from being new to C++ | AddressSanitizer in development builds, RAII discipline enforced by code structure, no raw owning pointers |
| Data races that Windows tooling cannot detect | Portable code plus ThreadSanitizer builds in Linux CI from phase 0 |
| The GUI slips to the end and never gets built | A minimal Qt window is a phase 3 deliverable |
| The optimisation arc becomes an open-ended rabbit hole | Each stage ends when its benchmark is recorded and its analysis written, not when it feels fast |

## Success criteria

The project is a success when all of the following hold:

1. A recorded sustained throughput figure with p50, p99 and p99.9 latency,
   reproducible via `switchd`'s benchmark mode.
2. Three benchmarked ledger stages with a written analysis of each change.
3. The crash-recovery test passing in CI.
4. The parser fuzzer running clean over millions of mutated inputs.
5. A Qt console showing live metrics, the transaction stream, and working fault
   injection.
6. A README that explains the architecture well enough for a payments engineer
   to understand the design in five minutes.
