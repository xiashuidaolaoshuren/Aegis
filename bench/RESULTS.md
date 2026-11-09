# Aegis benchmark results

Three ledger stages of the M4 concurrency arc. Every stage is measured with the same harness
(`aegisd --bench` server plus `aegis-load` client) and the same recipe, so the numbers compare
like for like.

## Shared recipe

| Setting | Value |
|---------|-------|
| Genesis | `fixtures/genesis_stage2.csv` (8 cardholders funded 1,000,000 minor units, merchant `m-1`, `system`) |
| Server workers | 8 |
| Server I/O threads | 1 |
| Client terminals | 32 |
| Client PANs | 8 (`--pans 8`) |
| Stage-3 partitions | 8 (`--partitions 8`) |
| Offered rate | 1000 req/s for 4 s (4000 requests) |
| Bench window | 5 s |

```powershell
aegisd --genesis fixtures/genesis_stage2.csv --port 8583 --workers 8 --ledger-stage <1|2|3> `
       [--partitions 8] --bench --bench-out bench/results/stage<1|2|3>.txt --bench-duration 5 `
       --bench-rate 1000 --bench-load-duration 4 --bench-terminals 32 --bench-pans 8
aegis-load --port 8583 --rate 1000 --duration 4 --terminals 32 --pans 8
```

`aegis-load` opens one connection per request, so the client caps the achievable rate at roughly
800 req/s. Every stage finishes all 4000 requests, which means this recipe measures latency, not
throughput: a higher-throughput comparison needs a client that reuses connections.

All three result files were re-measured together in one session on 2026-11-09. The earlier
stage-1/2 files came from a different environment, so the tables below use the re-measured
files to keep the three-way comparison like for like.

## Stage 1 — one engine-wide mutex

`Engine::io_loop` wraps the whole `Authorizer::handle` call in a single `ledger_mutex_`, so every
authorization queues behind the previous one, including its issuer wait.

**Result** ([results/stage1.txt](results/stage1.txt)):

| Metric | Value |
|--------|-------|
| Requests | 3900 |
| Throughput | 779 req/s |
| p50 | 1709 µs |
| p99 | 4771 µs |
| p99.9 | 8093 µs |

## Stage 2 — striped wallet locks

Stage 2 removes the engine-wide mutex and moves synchronization into the components:

- `StripedLocks` hashes a wallet to one of 16 stripes; verbs take the stripes they need in
  ascending index order, so a capture that pays a cardholder, a merchant, and the system wallet
  cannot deadlock with another capture.
- The hold metadata (hold map, next id, TTL, WAL appends) has its own mutex, and no verb ever
  holds it at the same time as a stripe: a hold is claimed (erased) under metadata, then posted
  under stripes. Deadlock is impossible by construction rather than by ordering.
- The idempotency store and the authorizer response cache gained internal locks and now return
  values instead of pointers into their maps; the issuer call counter became atomic.

**Result** ([results/stage2.txt](results/stage2.txt)):

| Metric | Value | vs stage 1 |
|--------|-------|------------|
| Requests | 3928 | — |
| Throughput | 786 req/s | +1 % |
| p50 | 616 µs | −64 % |
| p99 | 1615 µs | −66 % |
| p99.9 | 2837 µs | −65 % |

**What changed versus stage 1.** Latency drops across the distribution while throughput stays
flat. The flat throughput is the client ceiling described above, not the ledger: what improved is
how long an individual authorization waits, because a worker no longer queues behind another
worker's entire `handle` call. The effect is largest at the tail, which is where a serialized
request path hurts most: one slow request used to stall every request behind it.

**What stage 2 still leaves serialized.** The hold metadata mutex is taken by every reserve,
capture, reverse, and sweep, and a capture takes three stripes at once. Both are small critical
sections, but they are shared. Stage 3 removes them by giving each wallet partition a single
writer thread, so a partition needs no locks and the hold map can be sharded by partition.

## Stage 3 — wallet partitions + capture 2PC

Stage 3 removes the shared critical sections stage 2 left behind:

- Wallets hash into N partitions (8 here). Each partition owns a disjoint slice of wallets and a
  shard of the hold map, and runs on exactly one writer thread, so inside a partition nothing is
  shared and nothing is locked. The global hold-metadata mutex and the three-stripe capture path
  are gone.
- A verb is a message to the owning writer: the caller submits a task and waits for the result.
  Authorization touches only the cardholder's partition. A capture is the one cross-partition
  verb.
- Capture is a two-phase commit. The cardholder partition claims the hold and stages the debit;
  the merchant and system partitions validate and stage their credits. Every participant must
  prepare, in cardholder → merchant → system order with duplicate partitions merged. If any one
  refuses, they all roll back and the hold is restored. Only when all prepare does the ledger
  append a single capture record, and only then does each participant commit its staged lines
  exactly once.
- Hold ids encode the owning partition (`partition << 32 | local`), so capture and reverse route
  from the id alone without a directory of live holds.

**Result** ([results/stage3.txt](results/stage3.txt)):

| Metric | Value | vs stage 2 | vs stage 1 |
|--------|-------|------------|------------|
| Requests | 3979 | — | — |
| Throughput | 788 req/s | +0.3 % | +1 % |
| p50 | 727 µs | +18 % | −57 % |
| p99 | 1430 µs | −11 % | −70 % |
| p99.9 | 3300 µs | +16 % | −59 % |

**What changed versus stage 2.** Throughput is unchanged because the client caps it. p50 is about
18 % *worse* than stage 2: every verb now crosses to its partition's writer thread and back, which
costs two context switches, whereas stage 2's striped locks were almost always uncontended. The
first cut of the submit path read the partition TTL in a separate round trip, doubling that cost
(p50 1121 µs, p99 4135 µs); collapsing it to a single handoff is what produced the numbers above.
At the tail, p99 improves: a slow request can no longer queue behind a global metadata mutex or a
three-stripe capture, and each writer serializes only its own slice. p99.9 is slightly worse than
stage 2, consistent with the extra handoff dominating when the system is quiet.

**What the network bench does not exercise.** The ISO path currently carries authorizations only
(`0100`); capture, reversal, and expiry are ledger-level operations driven by the tests. This
recipe therefore measures the reserve path. Capture 2PC, rollback, WAL-at-commit, and the mixed
concurrent load are covered by `tests/ledger_stage3_2pc_test.cpp` against the shadow model. The
architectural gain of stage 3 is structural — no locks or shared metadata on the money path and a
sharded hold map — and this recipe cannot show it as a throughput win because the client is the
bottleneck at ~790 req/s. Raising the partition count trades p50 for tail isolation once the
offered load is high enough for contention to matter.
