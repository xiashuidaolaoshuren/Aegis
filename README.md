# Aegis

Aegis is a simulated card-authorization switch: ISO 8583 over TCP, a durable
double-entry ledger, and desktop operations consoles. This repository is built
in milestones; M1 (Foundation) ships the toolchain and ISO 8583 codec.

## Prerequisites

- **Visual Studio 2026 Build Tools** with the Desktop C++ workload (MSVC)
- **vcpkg** cloned and bootstrapped, with `VCPKG_ROOT` pointing at the clone
  (e.g. `C:\vcpkg`)
- **CMake** 3.21+ (bundled with Visual Studio is fine)

Open a fresh terminal after setting `VCPKG_ROOT` so presets can resolve
`$env{VCPKG_ROOT}`.

## Windows — configure, build, test

```powershell
cmake --preset windows-msvc
cmake --build --preset windows-msvc --config Debug
ctest --preset windows-msvc -C Debug
```

## Linux — configure, build, test

```bash
cmake --preset linux-clang
cmake --build --preset linux-clang
ctest --preset linux-clang --output-on-failure
```

## Linux CI and sanitizers

CI runs on push and pull requests via [`.github/workflows/ci.yml`](.github/workflows/ci.yml):

- **linux-clang** — Clang build and `ctest` on `ubuntu-latest`
- **linux-clang-asan** — same with AddressSanitizer (`linux-clang-asan` preset)
- **linux-clang-fuzz** — libFuzzer smoke on the ISO 8583 parser (30s)

To run the ASan preset locally on Linux:

```bash
cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ctest --preset linux-clang-asan --output-on-failure
```

ThreadSanitizer (TSan) is planned for M4 when concurrent engine code exists; M1
only ships the ASan preset.

## Property and fuzz tests

The ISO 8583 codec has a corpus-driven round-trip property test and a libFuzzer
harness. Seed files live in `fuzz/corpus/` (raw wire bytes, no TCP length
prefix). `ctest` reads them from the source tree on every platform.

Regenerate seeds after changing the codec:

```powershell
cmake --build --preset windows-msvc --config Debug --target gen_corpus
.\build\windows-msvc\fuzz\Debug\gen_corpus.exe fuzz/corpus
ctest --preset windows-msvc -C Debug -R Iso8583RoundTrip
```

On Linux, the fuzzer is an optional Clang target:

```bash
cmake --preset linux-clang -DAEGIS_BUILD_FUZZER=ON
cmake --build --preset linux-clang
./build/linux-clang/fuzz/gen_corpus fuzz/corpus
./build/linux-clang/fuzz/aegis_fuzz fuzz/corpus/ -max_total_time=30 -print_final_stats=1
```

CI adds a **linux-clang-fuzz** job that runs the same 30-second smoke. The
harness only requires that `parse` never crashes; malformed input is a
`Result` error.

## What's next

- **M2:** Ledger (double-entry, holds, WAL, crash recovery)

See [docs/superpowers/specs/2026-08-15-aegis-design.md](docs/superpowers/specs/2026-08-15-aegis-design.md)
for the full design.
