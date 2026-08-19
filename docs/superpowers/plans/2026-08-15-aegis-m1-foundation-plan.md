# Implementation plan: Aegis M1 — Foundation

**Spec:** [docs/superpowers/specs/2026-08-15-aegis-design.md](../specs/2026-08-15-aegis-design.md)
**Created:** 2026-08-15
**Subsystem scope:** Foundation (toolchain + ISO 8583 codec)
**Depends on:** —
**Next:** [M2 Ledger](2026-08-15-aegis-m2-ledger-plan.md)

## Summary

Ship a CMake/vcpkg project named `Aegis` that builds the `aegis` static library and GoogleTest on Windows (MSVC) and Linux CI, plus an ISO 8583 codec that round-trips the spec’s message types and fields, with a property test and a fuzzer smoke run. Out of scope: ledger, sockets, Qt, WebEngine, `issuersim`, observer.

## Discovery notes

- Reuse: empty repo except spec, `CONTEXT.md`, ADRs — no existing C++ sources.
- Constraints: C++20; CMake + `CMakePresets.json`; vcpkg manifest mode; GoogleTest; MSVC locally, Clang in CI; no Qt/WebEngine in this milestone.
- Patterns: `Result<T,E>` on the codec hot path; `Tagged<T,Tag>` for IDs; `Money` integer minor units + `Currency`; parser never crashes.
- Anti-goals: do not pull Qt “to get ready”; do not implement ledger “while here”; do not expand ISO field list.



## File map



### Subsystem: Foundation


| Path                               | Create/Modify | Responsibility                                                                                      | Public surface                             |
| ---------------------------------- | ------------- | --------------------------------------------------------------------------------------------------- | ------------------------------------------ |
| `CMakeLists.txt`                   | create        | Root project `Aegis`, C++20, add `aegis` + tests                                                    | CMake targets `aegis`, `aegis_tests`       |
| `CMakePresets.json`                | create        | Windows MSVC + Linux Clang configure/build/test                                                     | presets: `windows-msvc`, `linux-clang`     |
| `vcpkg.json`                       | create        | GoogleTest only                                                                                     | manifest                                   |
| `.gitignore`                       | create        | build/, vcpkg_installed/, IDE junk                                                                  | —                                          |
| `.clang-format`                    | create        | LLVM-ish C++20 style                                                                                | —                                          |
| `README.md`                        | create        | How to configure, build, test on Windows and CI                                                     | —                                          |
| `.github/workflows/ci.yml`         | create        | Linux Clang: configure, test, ASan, fuzzer smoke                                                    | CI job                                     |
| `aegis/CMakeLists.txt`             | create        | Library sources, public include dir                                                                 | target `aegis`                             |
| `aegis/result.hpp`                 | create        | `Result<T,E>` value type                                                                            | `ok`, `err`, `has_value`                   |
| `aegis/tagged.hpp`                 | create        | `Tagged<T, Tag>` strong typedef                                                                     | equality, hash if needed                   |
| `aegis/money.hpp`                  | create        | `Currency`, `Money` (int64 minor units)                                                             | add same-currency only (runtime err)       |
| `aegis/ids.hpp`                    | create        | `AccountId`, `MerchantId`, `TerminalId`, `Stan`, `Rrn`, `Pan`                                       | `Pan` format masks; explicit full accessor |
| `aegis/iso8583/fields.hpp`         | create        | `constexpr` field table (spec field list only)                                                      | field ids, encodings, lengths              |
| `aegis/iso8583/message.hpp`        | create        | MTI + present fields                                                                                | get/set field, MTI helpers                 |
| `aegis/iso8583/codec.hpp`          | create        | Parse/serialise over `std::span<const std::byte>`                                                   | `parse`, `serialise` → `Result`            |
| `tests/CMakeLists.txt`             | create        | GoogleTest executable                                                                               | target `aegis_tests`                       |
| `tests/money_ids_test.cpp`         | create        | Money mismatch error; Pan masking; Tagged mix-up does not compile (static check via separate types) | —                                          |
| `tests/iso8583_codec_test.cpp`     | create        | Parse/serialise each MTI and field in scope                                                         | —                                          |
| `tests/iso8583_roundtrip_test.cpp` | create        | Property: serialise(parse(bytes)) == original corpus                                                | —                                          |
| `fuzz/iso8583_fuzz.cpp`            | create        | libFuzzer harness: parse must not crash                                                             | —                                          |
| `fuzz/corpus/`                     | create        | Seed ISO messages                                                                                   | —                                          |




### Blast radius


| Path                                    | Why sensitive                                 | Plan mode (before implementation)                               |
| --------------------------------------- | --------------------------------------------- | --------------------------------------------------------------- |
| `CMakeLists.txt`, presets, `vcpkg.json` | Every later milestone builds on this layout   | high — confirm vcpkg + MSVC + Clang CI before codec depth       |
| `aegis/ids.hpp`, `money.hpp`            | Domain types used by every module             | high — lock `Pan` masking and `AccountId` = wallet              |
| `aegis/iso8583/codec.hpp`               | Parser must never crash; field list is frozen | high — confirm encodings (BCD vs ASCII) against spec field list |




## Workflow (for implementers)

1. **writing-plans** produced this file (type-1 decomposition only).
2. For each subtask: **Plan mode** + **planning-subtasks** → type-2 plan when **Plan mode** priority warrants it.
3. **Agent mode**: **test-driven-development** when `TDD suitable: yes` (or TDD slice of `partial`); match **Verification** when `no`.
4. Update this document if reality diverges; add a **Plan changelog** row.



## Subtasks



### T1 — Toolchain skeleton

- [x] **Do:** CMake project `Aegis`, presets, vcpkg (gtest), `.gitignore`, `.clang-format`, empty `aegis` lib that compiles, `aegis_tests` that runs one dummy test, README stub.

- **Blocked by:** —
- **Plan mode:** high
- **TDD suitable:** no
- **TDD suitable reason:** declarative config / wiring-only (CMake, vcpkg, CI files); no runtime behavior yet.
- **Verification:** Configure + build + `ctest` on Windows; Linux CI job green on a smoke test.



### T2 — CI Linux + ASan preset

- [x] **Do:** GitHub Actions (or equivalent) Clang build/test; AddressSanitizer-capable preset documented. No WebEngine.

- **Blocked by:** T1
- **Plan mode:** medium
- **TDD suitable:** no
- **TDD suitable reason:** CI YAML and CMake presets; no production behavior.
- **Verification:** Push or `act`-equivalent: Linux job compiles `aegis_tests` and runs them.



### T3 — Result, Tagged, Money, IDs including Pan mask

- [x] **Do:** Header-only (or small cpp) types: `Result`, `Tagged`, `Money`/`Currency`, wallet/terminal/STAN/RRN/`Pan` with masked format and explicit full-PAN accessor.

- **Blocked by:** T1
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `ctest -R money_ids` (or equivalent filter) all pass.



### T4 — ISO 8583 field table + message model

- [ ] **Do:** `constexpr` spec for fields 2,3,4,7,11–13,37–39,41,42,49,52,90 and MTIs 0100/0110, 0200/0210, 0400/0410, 0800/0810. Message object holds present bitmap fields.

- **Blocked by:** T3
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** Tests for field presence, MTI helpers, missing-field get returns error.



### T5 — Codec parse and serialise

- [ ] **Do:** Length-agnostic codec over `span`; malformed input → `Result` error, never exception/crash. Round-trip every in-scope field encoding.

- **Blocked by:** T4
- **Plan mode:** high
- **TDD suitable:** yes
- **Verification:** `iso8583_codec_test` covers each MTI and field; malformed truncated/garbage returns error.



### T6 — Round-trip property + fuzzer smoke

- [ ] **Do:** Corpus property test serialise(parse(x))==x; libFuzzer target + seed corpus; CI smoke (short timeout) must not crash.

- **Blocked by:** T5
- **Plan mode:** medium
- **TDD suitable:** partial
- **TDD suitable reason:** property tests are TDD; fuzzer harness is smoke/checklist not red/green on crashes.
- **Verification:** Property tests pass; CI fuzzer smoke exits 0; README documents how to run both.



## TDD note (Agent mode)

Per subtask, obey `TDD suitable`: `yes` means strict **test-driven-development** (red/green/refactor); `partial` applies it only to the testable slice; `no` means do not force test-first—still satisfy **Verification**.

## Plan changelog


| Date       | Change                                                                                                 |
| ---------- | ------------------------------------------------------------------------------------------------------ |
| 2026-08-15 | Initial plan                                                                                           |
| 2026-08-18 | Local Windows toolchain is VS Build Tools 2026; `windows-msvc` uses generator `Visual Studio 18 2026`. |
| 2026-08-19 | T4 adds `tests/iso8583_message_test.cpp` (omitted from original file map). |


