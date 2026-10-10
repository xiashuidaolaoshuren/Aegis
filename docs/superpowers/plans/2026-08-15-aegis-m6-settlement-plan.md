# Implementation plan: Aegis M6 — Settlement (stretch)

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Goal scope:** Batch reconciliation and merchant payout reports that still balance
**Depends on:** [M5 Dual Consoles](2026-08-15-aegis-m5-dual-consoles-plan.md)
**Next:** —

## Summary

Stretch only. Does **not** block M1–M5 success. Ingest a batch of captures vs authorizations, classify mismatches, compute merchant payouts (Payable discharge) with fees, reports that still balance. Out of scope unless this milestone is explicitly started: new ISO types, replication, real bank files.

## Discovery notes

- Reuse: M2 wallets and buckets; capture already credited Merchant Payable via `split_capture`; genesis and WAL.
- Constraints: Settlement **discharges** Payable; it is not Capture. No refunds. No reversal after capture.
- Patterns to follow: start with an in-memory batch exported from the engine. Parallel scan or mmap only if a real file shows up.
- Anti-goals: do not reopen partial capture or post-capture reversal.

## File map

### Goal: Settlement

| Path | Create/Modify | Responsibility | Public surface | Verified/Provisional |
|------|---------------|----------------|----------------|----------------------|
| `aegis/settlement/batch.hpp` | create | Batch record (auth vs capture keys, amounts) | structs | provisional |
| `aegis/settlement/match.hpp` | create | Match + mismatch classification | `reconcile` | provisional |
| `aegis/settlement/payout.hpp` | create | Discharge Payable; report lines | `settle_merchant` | provisional |
| `aegis/settlement/report.hpp` | create | Text/CSV report that balances to the ledger | `write_report` | provisional |
| `tests/settlement_match_test.cpp` | create | Matches, unmatched auth, unmatched capture | — | provisional |
| `tests/settlement_payout_test.cpp` | create | Payout totals vs Payable; still balanced | — | provisional |
| Optional console or Vue tab | modify | Show the last report | — | provisional |

### Blast radius

| Path | Why sensitive | Behavior that must stay intact |
|------|----------------|--------------------------------|
| `aegis/ledger/ledger.hpp` | A new settle posting must not break Hold/Capture | Settle does not touch Holds. It only discharges Merchant Payable. Capture and reverse stay as M2/M4 left them |
| Capture vs settlement language | Easy to mix with M2 capture | Settlement is not capture. Read `CONTEXT.md` Settlement vs Capture before T2 |

## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition).
2. Per subtask: **Plan mode** + **planning-subtasks** → **CreatePlan** type-2 plan when **Plan mode** is `high` (or `medium` with a real remaining unknown).
3. **Agent mode**: **test-driven-development** when **`TDD suitable: yes`** (or the TDD slice of **`partial`**); follow **Verification** when **`no`**.
4. Equivalent internal changes: record and continue. Material changes: pause and confirm, then log a **Plan changelog** row.

Do not start unless M5 is accepted as done. T2’s type-2 plan must name the clearing bucket (existing System wallet versus a new bucket) before any payout code.

## Subtasks

Dependency notation: `Blocked by: T1` means start after T1 is done.

### T1 — Reconcile batch vs ledger

- [ ] **Do:** Match authorization keys to captures; classify mismatches.
- **Consumes:** M2 `IdempotencyKey` (TerminalId + Stan + MMDD) and captured amounts
- **Produces:** `reconcile(batch) ->` matched rows plus mismatch classes. The class names are chosen in the type-2 plan; this plan does not invent them.
- **Acceptance:** A capture whose auth key is in the batch is a match. An authorization with no capture, and a capture with no authorization, are mismatches and are not silently dropped. Tests name each class the type-2 plan picks.
- **Compatibility:** Reconciliation does not post.
- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `settlement_match_test`.

### T2 — Payout postings + balanced report

- [ ] **Do:** Discharge Merchant Payable; report sums equal the ledger; no Hold changes.
- **Consumes:** T1 matched rows; M2 `PostingBatch` / `validate`; the clearing bucket named by the type-2 plan
- **Produces:** `settle_merchant` postings that move Merchant Payable to that clearing bucket; `write_report` text or CSV
- **Acceptance:** Payout totals equal the Payable that capture created, including the fee already taken at capture. Report sums equal ledger balances. Hold balances are unchanged. `assert_balanced` still holds.
- **Compatibility:** This is not a second capture and not a refund. Post-capture reversal stays forbidden.
- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `settlement_payout_test`; debug assert still debits==credits.

### T3 — Optional ops view

- [ ] **Do:** Show the last report in Widgets or Vue. Skip if time is gone.
- **Consumes:** T2 report text
- **Produces:** an extra tab or panel, or an explicit skip
- **Acceptance:** The last report is visible, or the changelog says T3 was skipped.
- **Blocked by:** T2
- **Plan mode:** skip
- **TDD suitable:** no
- **TDD suitable reason:** visual-only stretch.
- **Verification:** Manual screenshot, or skip recorded in the changelog.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**: **`yes`** means strict **test-driven-development** (red/green/refactor); **`partial`** applies it only to the testable slice; **`no`** means do not force test-first — still satisfy **Verification**. A behavior-preserving refactor with a covering suite is **`no`** (stay-green), not a new RED/GREEN pair. Type-2 planning → **planning-subtasks** skill. UI work is often **`partial`**: TDD the behavior core, verify polish and motion manually.

T1–T2 are **`yes`**. T3 is **`no`**.

## Plan changelog

| Date | Change | Kind |
|------|--------|------|
| 2026-08-15 | Initial plan | — |
| 2026-10-01 | Reshaped to the updated writing-plans scaffold. T1–T2 stay `high` (mismatch classes and the clearing bucket are still unnamed). T3 stays `skip`. TDD tags unchanged | equivalent |
