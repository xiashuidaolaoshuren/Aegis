# Implementation plan: Aegis M2 — Ledger

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**ADRs:** [0001 closed-loop](../../adr/0001-closed-loop-ledger.md), [0002 capture 2PC](../../adr/0002-capture-is-cross-wallet.md) (2PC itself is M4; posting shape is M2)
**Created:** 2026-08-15
**Goal scope:** Single-threaded double-entry ledger with holds, idempotency, and WAL replay
**Depends on:** [M1 Foundation](2026-08-15-aegis-m1-foundation-plan.md)
**Next:** [M3 Live Switch](2026-08-15-aegis-m3-live-switch-plan.md)

## Summary

Ship a single-threaded ledger inside `aegis`: wallets + buckets, genesis fixture, balanced postings, reserve/capture/reversal/TTL expiry, idempotency (TerminalId+STAN+field 7 date), WAL + crash replay vs shadow model. Out of scope: threads, TCP, Qt, striped locks, 2PC (capture is posted atomically on one thread here).

## Discovery notes

- Reuse: M1 `Money`, `AccountId`, `Pan`, `MerchantId`, `TerminalId`, `Stan`, `Result`. Idempotency keys use those IDs plus field-7 MMDD; they do not parse ISO inside the ledger.
- Constraints: closed-loop (Aegis is the only book); `AccountId` is a wallet; Available/Holds/Payable/Interchange are buckets; full capture only; no reversal after capture; genesis fixture, no ISO funding.
- Patterns to follow: PImpl on `Ledger`; RAII file for WAL; debug assert debits==credits after every batch; fee split in `split_capture` (29/1000 interchange).
- Anti-goals: no mutex “for later”; do not implement `issuersim`; do not start Qt.

## File map

### Goal: Ledger

| Path | Create/Modify | Responsibility | Public surface | Verified/Provisional |
|------|---------------|----------------|----------------|----------------------|
| `aegis/ledger/bucket.hpp` | create | Bucket enum | `Bucket::{Available, Holds, Payable, Interchange}` | verified |
| `aegis/ledger/wallet.hpp` | create | Wallet kind + `AccountId`; bucket balances | `WalletKind`, `balance(Bucket)` | verified |
| `aegis/ledger/posting.hpp` | create | Balanced posting batch | `PostingLine`, `PostingBatch`, `validate` → `Result<void, PostingError>`, `apply_batch` | verified |
| `aegis/ledger/hold.hpp` | create | Live Hold | `Hold`, `HoldId`, `TimePoint` | verified |
| `aegis/ledger/fee.hpp` | create | Capture fee split | `split_capture(Money) -> CaptureSplit{payable, interchange}` | verified |
| `aegis/ledger/idempotency.hpp` | create | Key = TerminalId + Stan + field7 MMDD | `IdempotencyKey`, `IdempotencyStore::{find,store}_{reserve,capture,reverse}`, `find_hold_id` | verified |
| `aegis/ledger/genesis.hpp` | create | Load opening Available from fixture | `load_genesis(path) -> Result<map<AccountId, Wallet>, GenesisError>` | verified |
| `aegis/ledger/wal.hpp` | create | Append-only WAL, replay | `append_reserve/capture/reverse`, `flush`, `Wal::replay(path, wallets) -> Result<Ledger, WalError>` | verified |
| `aegis/ledger/ledger.hpp` | create | PImpl ledger | `reserve`, `capture`, `reverse`, `expire_due`, `set_hold_ttl`, `set_wal`, `balance` | verified |
| `aegis/ledger/ledger.cpp` | create | Single-threaded implementation | — | verified |
| `aegis/CMakeLists.txt` | modify | Add ledger sources | — | verified |
| `tests/shadow_model.hpp` | create | Naive map wallet→buckets, same operations | oracle API parallel to ledger | verified |
| `tests/ledger_posting_test.cpp` | create | Balanced batches; reject unbalanced | — | verified |
| `tests/ledger_genesis_test.cpp` | create | Fixture load; unknown PAN is not auto-funded | — | verified |
| `tests/ledger_lifecycle_test.cpp` | create | Reserve, capture, reverse, expiry, idempotency | — | verified |
| `tests/ledger_crash_recovery_test.cpp` | create | WAL replay matches shadow after simulated kill | — | verified |
| `fixtures/genesis_tiny.csv` | create | Tiny wallet set for tests | CSV rows: kind, account, currency, available minor | verified |

### Blast radius

| Path | Why sensitive | Behavior that must stay intact |
|------|----------------|--------------------------------|
| `aegis/ledger/ledger.hpp` | Money movement API used by M3 authorizer and M4 stages | `reserve` / `capture` / `reverse` / `expire_due` signatures; single-threaded atomic capture; `51` is `InsufficientFunds` with no Hold posted |
| `tests/shadow_model.hpp` | Oracle for all later ledger rewrites | Stays a naive map. M4 must not “optimise the shadow” |
| `aegis/ledger/wal.hpp` | Crash recovery invariant | Replay rebuilds the same buckets as the shadow model; the WAL does not drop records |

## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition).
2. Per subtask: **Plan mode** + **planning-subtasks** → **CreatePlan** type-2 plan when **Plan mode** is `high` (or `medium` with a real remaining unknown).
3. **Agent mode**: **test-driven-development** when **`TDD suitable: yes`** (or the TDD slice of **`partial`**); follow **Verification** when **`no`**.
4. Equivalent internal changes: record and continue. Material changes: pause and confirm, then log a **Plan changelog** row.

This milestone is done. M3 calls `Ledger` on one writer thread; M4 may change the locking strategy but not these verb signatures without a material plan change.

## Subtasks

Dependency notation: `Blocked by: T1` means start after T1 is done.

### T1 — Posting batch and wallet buckets

- [x] **Do:** Wallet + buckets + posting batch that must sum to zero; debug assert helper.
- **Consumes:** M1 `Money`, `AccountId`, `Result`
- **Produces:** `Bucket`; `Wallet`; `PostingBatch`; `validate(batch) -> Result<void, PostingError>` (`Unbalanced`); `apply_batch`; `assert_balanced`
- **Acceptance:** A balanced batch applies. An unbalanced batch returns `Unbalanced` and does not change balances. Debug `assert_balanced` fires on an unbalanced batch.
- **Compatibility:** Bucket names stay Available, Holds, Payable, Interchange.
- **Blocked by:** —
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `ledger_posting_test` — balanced apply; unbalanced rejected.

### T2 — Genesis fixture

- [x] **Do:** Load Cardholder/Merchant/System wallets with opening Available (tests: tiny CSV fixture).
- **Consumes:** T1 `Wallet`, `Bucket::Available`
- **Produces:** `load_genesis(path) -> Result<unordered_map<AccountId, Wallet>, GenesisError>`; `fixtures/genesis_tiny.csv`
- **Acceptance:** The tiny fixture loads cardholder, merchant, and system wallets with the stated Available minor units. A PAN that is not in the file does not create a funded wallet. Bad CSV returns `ParseError` or `IoFailure`.
- **Blocked by:** T1
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `ledger_genesis_test` loads the fixture; unknown PAN is not auto-funded.

### T3 — Reserve (Hold) and funds check 51

- [x] **Do:** Intra-wallet Available→Holds; insufficient funds if Available would go negative; no Hold posted on that failure.
- **Consumes:** T2 wallets; `Money`
- **Produces:** `Ledger::reserve(AccountId, Money, MerchantId, TimePoint) -> Result<Hold, LedgerError>`; `LedgerError::InsufficientFunds` is the `51` case
- **Acceptance:** Successful reserve moves the amount from Available to Holds and returns a `Hold` with id, merchant, amount, and expiry. If Available would go negative, the result is `InsufficientFunds`, balances are unchanged, and `live_hold_count` does not increase.
- **Compatibility:** Authorization stays single-wallet. M4 2PC does not apply to reserve.
- **Blocked by:** T2
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** Lifecycle tests for insufficient funds vs successful reserve.

### T4 — Capture full amount + fee split

- [x] **Do:** Consume entire Hold; credit Merchant Payable and System Interchange; reject amount ≠ original; reject missing Hold; intra-process atomic (single thread).
- **Consumes:** T3 `Hold` / `HoldId`; `split_capture`
- **Produces:** `Ledger::capture(HoldId, Money) -> Result<void, LedgerError>`; `split_capture` uses interchange = `minor * 29 / 1000`, payable = remainder
- **Acceptance:** Capturing 5000 minor units yields payable 4855 and interchange 145, Hold is gone, and the batch balances. Amount ≠ original returns `AmountMismatch`. Unknown id returns `UnknownHold`. No partial capture.
- **Compatibility:** No reversal after a successful capture. M4 stage 3 keeps this money outcome while changing how the wallets are locked.
- **Blocked by:** T3
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** Lifecycle capture test: 50.00 → 48.55 payable + 1.45 interchange; leftover Hold count is zero.

### T5 — Reverse and TTL expiry

- [x] **Do:** Holds→Available; forbidden after capture; sweeper `expire_due(now)`; configurable TTL.
- **Consumes:** T3 `reserve`; T4 rule that capture consumes the Hold
- **Produces:** `Ledger::reverse(HoldId) -> Result<void, LedgerError>`; `expire_due(TimePoint) -> size_t`; `set_hold_ttl(seconds)`
- **Acceptance:** Reverse and expiry move Holds back to Available. Reverse of a captured or unknown Hold fails and does not change buckets. `expire_due` returns the number of holds released and ignores holds that have not expired.
- **Blocked by:** T3
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** Reverse/expiry tests; capture-then-reverse fails.

### T6 — Idempotency store

- [x] **Do:** Key TerminalId+STAN+MMDD; duplicate reserve returns stored outcome without a second Hold; capture/reverse keyed on their own triple; field 90 locates the original Hold.
- **Consumes:** T4 `capture`, T5 `reverse`, M1 `TerminalId` and `Stan`
- **Produces:** `IdempotencyKey{terminal, stan, Mmdd}`; `IdempotencyStore::find_reserve/store_reserve`, `find_capture/store_capture`, `find_reverse/store_reverse`, `find_hold_id(original auth key)`
- **Acceptance:** A second reserve with the same key returns the stored `Result<Hold, LedgerError>` and does not post another Hold. Capture and reverse duplicates return the stored outcome. `find_hold_id` returns the Hold id stored for a successful reserve.
- **Compatibility:** The store is outside `Ledger`. Callers (tests now, M3 authorizer later) check it before posting.
- **Blocked by:** T4, T5
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `ledger_lifecycle_test` duplicate reserve, capture, and reverse cases.

### T7 — WAL + shadow crash recovery

- [x] **Do:** Append postings/holds to WAL; replay rebuilds buckets; test closes without treating the unflushed tail as durable and compares the shadow model.
- **Consumes:** T6 outcomes (replay must restore holds and captures the store would see); `shadow_model`
- **Produces:** `Wal::append_reserve`, `append_capture`, `append_reverse`, `flush`; `Wal::replay(path, wallets) -> Result<Ledger, WalError>`
- **Acceptance:** After a simulated kill, replayed bucket balances equal the shadow model. A corrupt record returns `WalError::CorruptRecord`. Unflushed bytes are not recovered.
- **Compatibility:** M4 stage changes must still match this shadow model, including captures.
- **Blocked by:** T6
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `ledger_crash_recovery_test` — replayed buckets == shadow.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**: **`yes`** means strict **test-driven-development** (red/green/refactor); **`partial`** applies it only to the testable slice; **`no`** means do not force test-first — still satisfy **Verification**. A behavior-preserving refactor with a covering suite is **`no`** (stay-green), not a new RED/GREEN pair. Type-2 planning → **planning-subtasks** skill. UI work is often **`partial`**: TDD the behavior core, verify polish and motion manually.

Every subtask in this milestone is **`yes`**.

## Plan changelog

| Date | Change | Kind |
|------|--------|------|
| 2026-08-15 | Initial plan | — |
| 2026-10-01 | Reshaped to the updated writing-plans scaffold. File map matches the tree: `fixtures/genesis_tiny.csv`, `fee.hpp`, `ledger_genesis_test.cpp`. T6 marked done — `IdempotencyStore` and duplicate lifecycle tests are already in the repo. Completed subtasks are `skip`. TDD tags unchanged | equivalent |
