# Aegis benchmark results

Three ledger stages (M4). Stage 1 uses a single global ledger writer (M3 topology).

## Stage 1 — single writer (M3)

**Load recipe:** `fixtures/genesis_tiny.csv`, workers 4, terminals 2, 20 req/s for 2 s (port 8583).

**Notes:** Genesis Available is 10000 minor units; each authorization is 1000, so about ten approvals (`39=00`) land before insufficient funds (`39=51`). The remainder of the run is declines with no new holds.

**Numbers:** see [results/stage1.txt](results/stage1.txt).

**Analysis (stub):** Stage 2 and 3 write-ups will compare against this baseline. Contention is expected on the single `ledger_mutex_` under parallel workers.

## Stage 2 — striped locks

*(Pending M4 T3.)*

## Stage 3 — wallet partitions + capture 2PC

*(Pending M4 T4.)*
