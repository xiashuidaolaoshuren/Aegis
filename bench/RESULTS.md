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
| Offered rate | 1000 req/s for 4 s (4000 requests) |
| Bench window | 5 s |

```powershell
aegisd --genesis fixtures/genesis_stage2.csv --port 8583 --workers 8 --ledger-stage <1|2> `
       --bench --bench-out bench/results/stage<1|2>.txt --bench-duration 5 `
       --bench-rate 1000 --bench-load-duration 4 --bench-terminals 32 --bench-pans 8
aegis-load --port 8583 --rate 1000 --duration 4 --terminals 32 --pans 8
```

`aegis-load` opens one connection per request, so the client caps the achievable rate at roughly
800 req/s. Both stages finish all 4000 requests, which means this recipe measures latency, not
throughput: a higher-throughput comparison needs a client that reuses connections.

## Stage 1 — one engine-wide mutex

`Engine::io_loop` wraps the whole `Authorizer::handle` call in a single `ledger_mutex_`, so every
authorization queues behind the previous one, including its issuer wait.

**Result** ([results/stage1.txt](results/stage1.txt)):

| Metric | Run A | Run B (committed) |
|--------|-------|-------------------|
| Requests | 3998 | 4000 |
| Throughput | 795 req/s | 798 req/s |
| p50 | 972 µs | 1031 µs |
| p99 | 1752 µs | 2070 µs |
| p99.9 | 2360 µs | 3494 µs |

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

| Metric | Run A | Run B (committed) | Run B vs stage 1 |
|--------|-------|-------------------|------------------|
| Requests | 4000 | 4000 | — |
| Throughput | 797 req/s | 798 req/s | unchanged |
| p50 | 590 µs | 601 µs | −42 % |
| p99 | 1202 µs | 1141 µs | −45 % |
| p99.9 | 1681 µs | 1460 µs | −58 % |

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

*(Pending M4 T4.)*
