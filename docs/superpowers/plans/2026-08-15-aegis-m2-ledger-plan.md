# Implementation plan: Aegis M2 — Ledger

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**ADRs:** [0001 closed-loop](../../adr/0001-closed-loop-ledger.md), [0002 capture 2PC](../../adr/0002-capture-is-cross-wallet.md) (2PC itself is M4; posting shape is M2)
**Created:** 2026-08-15
**Subsystem scope:** Single-threaded double-entry ledger
**Depends on:** [M1 Foundation](2026-08-15-aegis-m1-foundation-plan.md)
**Next:** [M3 Live Switch](2026-08-15-aegis-m3-live-switch-plan.md)

## Summary

Ship a single-threaded ledger inside `aegis`: wallets + buckets, genesis fixture, balanced postings, reserve/capture/reversal/TTL expiry, idempotency (TerminalId+STAN+field 7 date), WAL + crash replay vs shadow model. Out of scope: threads, TCP, Qt, striped locks, 2PC (capture is posted atomically on one thread here).

## Discovery notes

- Reuse: M1 `Money`, `AccountId`, `Pan`, `MerchantId`, `Result`, codec types for STAN/field 7 if needed by idempotency keys.
- Constraints: closed-loop (Aegis is the only book); `AccountId` is a wallet; Available/Holds/Payable/Interchange are buckets; full capture only; no reversal after capture; genesis fixture, no ISO funding.
- Patterns: PImpl on ledger; RAII file for WAL; debug assert debits==credits after every batch.
- Anti-goals: no mutex “for later”; do not implement `issuersim`; do not start Qt.

## File map

### Subsystem: Ledger

| Path | Create/Modify | Responsibility | Public surface |
|------|----------------|----------------|----------------|
| `aegis/ledger/bucket.hpp` | create | Bucket enum: Available, Holds, Payable, Interchange | enum |
| `aegis/ledger/wallet.hpp` | create | Wallet kind + `AccountId`; bucket balances | `balance(Bucket)` |
| `aegis/ledger/posting.hpp` | create | Balanced posting batch (debit/credit lines) | `PostingBatch`, validate sum 0 |
| `aegis/ledger/hold.hpp` | create | Live Hold: amount, TTL, original auth key, merchant | `Hold` |
| `aegis/ledger/idempotency.hpp` | create | Key = TerminalId + Stan + field7 MMDD | `IdempotencyKey` |
| `aegis/ledger/genesis.hpp` | create | Load opening Available from fixture | `load_genesis(path or in-memory)` |
| `aegis/ledger/wal.hpp` | create | Append-only WAL, replay | `append`, `replay` |
| `aegis/ledger/ledger.hpp` | create | PImpl ledger: reserve, capture, reverse, expire | `reserve`, `capture`, `reverse`, `expire_due` |
| `aegis/ledger/ledger.cpp` | create | Single-threaded implementation | — |
| `aegis/CMakeLists.txt` | modify | Add ledger sources | — |
| `tests/shadow_model.hpp` | create | Naive map wallet→buckets, same operations | oracle API parallel to ledger |
| `tests/ledger_posting_test.cpp` | create | Balanced batches; reject unbalanced | — |
| `tests/ledger_lifecycle_test.cpp` | create | Genesis, 51, reserve, full capture fee split, reverse, expiry, no reverse after capture, idempotency | — |
| `tests/ledger_crash_recovery_test.cpp` | create | WAL replay matches shadow after simulated kill | — |
| `fixtures/genesis_tiny.json` (or csv) | create | Tiny wallet set for tests | — |

### Blast radius

| Path | Why sensitive | Plan mode (before implementation) |
|------|----------------|-----------------------------------|
| `aegis/ledger/ledger.hpp` | Money movement API used by M3 authorizer and M4 stages | high — lock signatures to domain verbs (reserve/capture/reverse/expire) |
| `tests/shadow_model.hpp` | Oracle for all later ledger rewrites | high — keep deliberately naive; no “optimise the shadow” |
| `aegis/ledger/wal.hpp` | Crash recovery invariant | high — define record format before capture/expiry |

## Workflow (for implementers)

1. Type-1 plan (this file).
2. Type-2 via **planning-subtasks** when Plan mode is high/medium.
3. Agent mode + TDD per tag.
4. Changelog if the file map is wrong.

## Subtasks

### T1 — Posting batch and wallet buckets

- [ ] **Do:** Wallet + buckets + posting batch that must sum to zero; debug assert helper.
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `ledger_posting_test` — balanced apply; unbalanced rejected.

### T2 — Genesis fixture

- [ ] **Do:** Load Cardholder/Merchant/System wallets with opening Available (tests: tiny fixture).
- **Blocked by:** T1
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** Test loads fixture; unknown PAN is not auto-funded.

### T3 — Reserve (Hold) and funds check 51

- [ ] **Do:** Intra-wallet Available→Holds; `51` if Available would go negative; no Hold posted on 51.
- **Blocked by:** T2
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** Lifecycle tests for 51 vs successful reserve.

### T4 — Capture full amount + fee split

- [ ] **Do:** Consume entire Hold; credit Merchant Payable and System Interchange; reject amount ≠ original; reject missing Hold; intra-process atomic (single thread).
- **Blocked by:** T3
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** 50.00 → 48.55 payable + 1.45 interchange (or spec fee formula encoded in one helper); leftover Hold is zero.

### T5 — Reverse and TTL expiry

- [ ] **Do:** Holds→Available; forbidden after capture; sweeper `expire_due(now)`; configurable TTL.
- **Blocked by:** T3
- **Plan mode:** medium
- **TDD suitable:** yes
- **Verification:** Reverse/expiry tests; capture-then-reverse fails.

### T6 — Idempotency store

- [ ] **Do:** Key TerminalId+STAN+MMDD; duplicate reserve returns stored outcome without second Hold; capture/reverse keyed on their own triple; field 90 locates original Hold.
- **Blocked by:** T4, T5
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** Duplicate 0100/0200/0400 tests.

### T7 — WAL + shadow crash recovery

- [ ] **Do:** Append postings/holds to WAL; replay rebuilds buckets; test kills logically (close without flush vs replay) and compares shadow model.
- **Blocked by:** T6
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `ledger_crash_recovery_test` — replayed buckets == shadow.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**. This milestone is almost entirely **`yes`**.

## Plan changelog

| Date | Change |
|------|--------|
| 2026-08-15 | Initial plan |
