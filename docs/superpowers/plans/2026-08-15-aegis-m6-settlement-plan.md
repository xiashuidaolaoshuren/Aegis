# Implementation plan: Aegis M6 — Settlement (stretch)

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Subsystem scope:** Batch reconciliation and merchant payout reports
**Depends on:** [M5 Dual Consoles](2026-08-15-aegis-m5-dual-consoles-plan.md)
**Next:** —

## Summary

Stretch only. Does **not** block M1–M5 success. Ingest a batch of captures vs authorizations, classify mismatches, compute merchant payouts (Payable discharge) with fees, reports that still balance. Out of scope unless this milestone is explicitly started: new ISO types, replication, real bank files.

## Discovery notes

- Reuse: ledger wallets/buckets; capture already created Merchant Payable; genesis and WAL.
- Constraints: Settlement **discharges** Payable; it is not Capture. No refunds.
- Patterns: parallel batch / mmap if data is large — only if a real file exists; start with in-memory batch from engine export.
- Anti-goals: do not reopen partial capture or post-capture reversal.

## File map

### Subsystem: Settlement

| Path | Create/Modify | Responsibility | Public surface |
|------|----------------|----------------|----------------|
| `aegis/settlement/batch.hpp` | create | Batch record (auth vs capture keys, amounts) | structs |
| `aegis/settlement/match.hpp` | create | Match + mismatch classification | `reconcile` |
| `aegis/settlement/payout.hpp` | create | Discharge Payable; report lines | `settle_merchant` |
| `aegis/settlement/report.hpp` | create | Text/CSV report that balances to ledger | `write_report` |
| `tests/settlement_match_test.cpp` | create | Matches, unmatched auth, unmatched capture | — |
| `tests/settlement_payout_test.cpp` | create | Payout totals vs Payable; still balanced | — |
| Optional UI hooks | modify | Extra tab in console — only if time | — |

### Blast radius

| Path | Why sensitive | Plan mode (before implementation) |
|------|----------------|-----------------------------------|
| `aegis/ledger/ledger.hpp` | New “settle” posting must not break Hold/Capture invariants | high — settle is Holds-forbidden; only Payable → (cash/clearing bucket TBD in type-2) |
| Capture vs settlement language | Easy to mix with M2 capture | high — read CONTEXT.md Settlement vs Capture |

## Workflow (for implementers)

Do not start unless M5 is accepted as done. Type-2 must name the clearing bucket (genesis System wallet vs new bucket) before T2.

## Subtasks

### T1 — Reconcile batch vs ledger

- [ ] **Do:** Match authorization keys to captures; classify mismatches.
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `settlement_match_test`.

### T2 — Payout postings + balanced report

- [ ] **Do:** Discharge Merchant Payable; report sums equal ledger; no Hold changes.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `settlement_payout_test`; debug assert still debits==credits.

### T3 — Optional ops view

- [ ] **Do:** Show last report in Widgets or Vue. Skip if time is gone.
- **Blocked by:** T2
- **Plan mode:** skip
- **TDD suitable:** no
- **TDD suitable reason:** visual-only stretch.
- **Verification:** Manual screenshot or skip.

## TDD note (Agent mode)

T1–T2 **`yes`**. T3 **`no`**.

## Plan changelog

| Date | Change |
|------|--------|
| 2026-08-15 | Initial plan |
