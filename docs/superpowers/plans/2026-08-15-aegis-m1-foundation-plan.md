# Implementation plan: Aegis M1 — Foundation

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Goal scope:** A buildable `aegis` library with an ISO 8583 codec that round-trips the spec’s message types and fields
**Depends on:** —
**Next:** [M2 Ledger](2026-08-15-aegis-m2-ledger-plan.md)

## Summary

Ship a CMake/vcpkg project named `Aegis` that builds the `aegis` static library and GoogleTest on Windows (MSVC) and Linux CI, plus an ISO 8583 codec that round-trips the spec’s message types and fields, with a property test and a fuzzer smoke run. Out of scope: ledger, sockets, Qt, WebEngine, `issuersim`, observer.

## Discovery notes

- Reuse: empty repo except spec, `CONTEXT.md`, ADRs — no existing C++ sources at planning time. Shipped headers below are now the contract M2 consumes.
- Constraints: C++20; CMake + `CMakePresets.json`; vcpkg manifest mode; GoogleTest; MSVC locally (`Visual Studio 18 2026`), Clang in CI; no Qt/WebEngine in this milestone.
- Patterns to follow: `Result<T,E>` on the codec hot path; `Tagged<T,Tag>` for IDs; `Money` integer minor units + `Currency`; parser never crashes.
- Anti-goals: do not pull Qt “to get ready”; do not implement ledger “while here”; do not expand the ISO field list.

## File map

### Goal: Foundation

| Path | Create/Modify | Responsibility | Public surface | Verified/Provisional |
|------|---------------|----------------|----------------|----------------------|
| `CMakeLists.txt` | create | Root project `Aegis`, C++20, add `aegis` + tests | CMake targets `aegis`, `aegis_tests` | verified |
| `CMakePresets.json` | create | Windows MSVC + Linux Clang configure/build/test | presets: `windows-msvc`, `linux-clang` | verified |
| `vcpkg.json` | create | GoogleTest only | manifest | verified |
| `.gitignore` | create | build/, vcpkg_installed/, IDE junk | — | verified |
| `.clang-format` | create | LLVM-ish C++20 style | — | verified |
| `README.md` | create | How to configure, build, test on Windows and CI | — | verified |
| `.github/workflows/ci.yml` | create | Linux Clang: configure, test, ASan, fuzzer smoke | CI job | verified |
| `aegis/CMakeLists.txt` | create | Library sources, public include dir | target `aegis` | verified |
| `aegis/result.hpp` | create | `Result<T,E>` value type | `ok`, `err`, `has_value` | verified |
| `aegis/tagged.hpp` | create | `Tagged<T, Tag>` strong typedef | equality; `std::hash` specialization | verified |
| `aegis/money.hpp` | create | `Currency`, `Money` (int64 minor units) | `Money::add` same-currency only → `Result<Money, MoneyError>` | verified |
| `aegis/ids.hpp` | create | Wallet and message IDs, masked PAN | `AccountId`, `MerchantId`, `TerminalId`, `Stan`, `Rrn`; `Pan::masked()`, `Pan::full()` | verified |
| `aegis/iso8583/fields.hpp` | create | `constexpr` field table (spec field list only) | `FieldId`, `Mti`, `kFieldTable`, `Encoding`, `LengthKind` | verified |
| `aegis/iso8583/message.hpp` | create | MTI + present fields | `Message(Mti)`, `has`, `get` → `Result<string, MessageError>`, `set` | verified |
| `aegis/iso8583/codec.hpp` | create | Parse/serialise over `std::span<const std::byte>` | `parse` → `Result<Message, CodecError>`; `serialise` → `Result<vector<byte>, CodecError>` | verified |
| `tests/CMakeLists.txt` | create | GoogleTest executable | target `aegis_tests` | verified |
| `tests/money_ids_test.cpp` | create | Money mismatch; Pan masking; distinct ID types | — | verified |
| `tests/iso8583_message_test.cpp` | create | Field presence and MTI helpers | — | verified |
| `tests/iso8583_codec_test.cpp` | create | Parse/serialise each MTI and field in scope | — | verified |
| `tests/iso8583_roundtrip_test.cpp` | create | Property: serialise(parse(bytes)) == original corpus | — | verified |
| `fuzz/CMakeLists.txt` | create | Corpus generator always; fuzzer target Clang-only | `gen_corpus`; `aegis_fuzz` behind `AEGIS_BUILD_FUZZER` | verified |
| `fuzz/gen_corpus.cpp` | create | Writes seed ISO messages | — | verified |
| `fuzz/iso8583_fuzz.cpp` | create | libFuzzer harness: parse must not crash | — | verified |
| `fuzz/corpus/` | create | Seed ISO messages | — | verified |

### Blast radius

| Path | Why sensitive | Behavior that must stay intact |
|------|----------------|--------------------------------|
| `CMakeLists.txt`, presets, `vcpkg.json` | Every later milestone builds on this layout | `aegis` stays a Qt-free static library; `aegis_tests` runs headless |
| `aegis/ids.hpp`, `money.hpp` | Domain types used by every module | `AccountId` is the wallet id; `Pan::masked()` hides the middle; `Money::add` errors on currency mismatch |
| `aegis/iso8583/codec.hpp` | Parser must never crash; field list is frozen | Malformed input returns `CodecError`; in-scope fields round-trip; no extra field ids |

## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition).
2. Per subtask: **Plan mode** + **planning-subtasks** → **CreatePlan** type-2 plan when **Plan mode** is `high` (or `medium` with a real remaining unknown).
3. **Agent mode**: **test-driven-development** when **`TDD suitable: yes`** (or the TDD slice of **`partial`**); follow **Verification** when **`no`**.
4. Equivalent internal changes: record and continue. Material changes: pause and confirm, then log a **Plan changelog** row.

This milestone is done. Later milestones consume the public surface above; do not reopen these subtasks unless a material contract change is confirmed.

## Subtasks

Dependency notation: `Blocked by: T1` means start after T1 is done.

### T1 — Toolchain skeleton

- [x] **Do:** CMake project `Aegis`, presets, vcpkg (gtest), `.gitignore`, `.clang-format`, empty `aegis` lib that compiles, `aegis_tests` that runs one dummy test, README stub.
- **Consumes:** none
- **Produces:** targets `aegis` and `aegis_tests`; presets `windows-msvc` (generator `Visual Studio 18 2026`) and `linux-clang`
- **Acceptance:** Configure, build, and `ctest` succeed on Windows. The dummy test passes. No Qt dependency.
- **Compatibility:** Later milestones keep this preset and target layout.
- **Blocked by:** —
- **Plan mode:** skip
- **TDD suitable:** no
- **TDD suitable reason:** declarative config / wiring-only (CMake, vcpkg); no runtime behavior yet.
- **Verification:** Configure + build + `ctest` on Windows; Linux CI job green on a smoke test.

### T2 — CI Linux + ASan preset

- [x] **Do:** GitHub Actions Clang build/test; AddressSanitizer-capable preset documented. No WebEngine.
- **Consumes:** T1 presets and `aegis_tests`
- **Produces:** `.github/workflows/ci.yml` Linux job that configures, builds, and runs tests with ASan available
- **Acceptance:** Linux job compiles `aegis_tests` and runs them. Workflow does not install Qt or WebEngine.
- **Blocked by:** T1
- **Plan mode:** skip
- **TDD suitable:** no
- **TDD suitable reason:** CI YAML and CMake presets; no production behavior.
- **Verification:** Linux CI job compiles `aegis_tests` and runs them.

### T3 — Result, Tagged, Money, IDs including Pan mask

- [x] **Do:** Header-only types: `Result`, `Tagged`, `Money`/`Currency`, wallet/terminal/STAN/RRN/`Pan` with masked format and explicit full-PAN accessor.
- **Consumes:** T1 `aegis` include layout
- **Produces:** `Result<T,E>::ok/err/has_value`; `Tagged<T,Tag>`; `Money::add` → `Result<Money, MoneyError>` (`MismatchedCurrency`); `Pan::masked()` / `Pan::full()`; `AccountId` as the wallet id
- **Acceptance:** Adding two `Money` values of different currencies returns `MismatchedCurrency` and does not produce a value. `Pan::masked()` keeps the first 6 and last 4 when the PAN is longer than 10. `AccountId` and `MerchantId` are different types.
- **Compatibility:** These names are the domain contract for M2–M6.
- **Blocked by:** T1
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `ctest -R money_ids` all pass.

### T4 — ISO 8583 field table + message model

- [x] **Do:** `constexpr` spec for fields 2, 3, 4, 7, 11–13, 37–39, 41, 42, 49, 52, 90 and MTIs 0100/0110, 0200/0210, 0400/0410, 0800/0810. Message object holds present bitmap fields.
- **Consumes:** T3 `Result`
- **Produces:** `kFieldTable` (15 specs: BCD LLVAR PAN; BCD fixed 3/4/7/11/12/13/49/90; ASCII fixed 37/38/39/41/42; binary fixed 52); `Message::has/get/set`; `MessageError::{FieldAbsent, UnknownField, InvalidMti}`
- **Acceptance:** `get` on a missing field returns `FieldAbsent`. `set` of an id outside the table returns `UnknownField`. Each in-scope MTI constructs a `Message`.
- **Compatibility:** Field list stays frozen; do not add fields in later milestones without a material plan change.
- **Blocked by:** T3
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `iso8583_message_test` covers field presence, MTI helpers, and missing-field `get`.

### T5 — Codec parse and serialise

- [x] **Do:** Length-agnostic codec over `span`; malformed input → `Result` error, never exception/crash. Round-trip every in-scope field encoding.
- **Consumes:** T4 `Message`, `kFieldTable`
- **Produces:** `parse(span<const byte>) -> Result<Message, CodecError>`; `serialise(const Message&) -> Result<vector<byte>, CodecError>`; `CodecError` includes `Truncated`, `Malformed`, `UnknownMti`, `UnsupportedField`
- **Acceptance:** Each in-scope MTI and field round-trips. Truncated or garbage input returns `CodecError` and does not throw.
- **Compatibility:** `parse` / `serialise` signatures stay stable for the fuzzer and for M3 framing.
- **Blocked by:** T4
- **Plan mode:** skip
- **TDD suitable:** yes
- **Verification:** `iso8583_codec_test` covers each MTI and field; malformed truncated/garbage returns error.

### T6 — Round-trip property + fuzzer smoke

- [x] **Do:** Corpus property test serialise(parse(x))==x; libFuzzer target + seed corpus; CI smoke (short timeout) must not crash.
- **Consumes:** T5 `parse` and `serialise`
- **Produces:** `iso8583_roundtrip_test`; `fuzz/gen_corpus` (always built); `aegis_fuzz` only when `AEGIS_BUILD_FUZZER` (Clang); seed files under `fuzz/corpus/`
- **Acceptance:** For each corpus file, `serialise(parse(bytes))` equals the original bytes. Fuzzer smoke exits 0. README says how to run both.
- **Blocked by:** T5
- **Plan mode:** skip
- **TDD suitable:** partial
- **TDD suitable reason:** property tests are TDD; fuzzer harness is smoke/checklist, not red/green on crashes.
- **Verification:** Property tests pass; CI fuzzer smoke exits 0; README documents how to run both.

## TDD note (Agent mode)

Per subtask, obey **`TDD suitable`**: **`yes`** means strict **test-driven-development** (red/green/refactor); **`partial`** applies it only to the testable slice; **`no`** means do not force test-first — still satisfy **Verification**. A behavior-preserving refactor with a covering suite is **`no`** (stay-green), not a new RED/GREEN pair. Type-2 planning → **planning-subtasks** skill. UI work is often **`partial`**: TDD the behavior core, verify polish and motion manually.

T3–T5 are **`yes`**. T6 is **`partial`** (property tests only). T1–T2 are **`no`**.

## Plan changelog

| Date | Change | Kind |
|------|--------|------|
| 2026-08-15 | Initial plan | — |
| 2026-08-18 | Local Windows toolchain is VS Build Tools 2026; `windows-msvc` uses generator `Visual Studio 18 2026` | equivalent |
| 2026-08-19 | T4 adds `tests/iso8583_message_test.cpp` (omitted from original file map) | equivalent |
| 2026-08-22 | T6: `fuzz/gen_corpus` always builds; `aegis_fuzz` is Clang-only behind `AEGIS_BUILD_FUZZER` | equivalent |
| 2026-10-01 | Reshaped to the updated writing-plans scaffold (goal scope, verified paths, consumes/produces/acceptance). Completed subtasks are `skip`: the handoff is recorded and nothing is left to decide. TDD tags unchanged | equivalent |
