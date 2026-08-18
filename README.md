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

To run the ASan preset locally on Linux:

```bash
cmake --preset linux-clang-asan
cmake --build --preset linux-clang-asan
ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 ctest --preset linux-clang-asan --output-on-failure
```

ThreadSanitizer (TSan) is planned for M4 when concurrent engine code exists; M1
only ships the ASan preset.

## What's next

- **T3+:** Domain types (`Result`, `Money`, IDs) and ISO 8583 codec

See [docs/superpowers/specs/2026-08-15-aegis-design.md](docs/superpowers/specs/2026-08-15-aegis-design.md)
for the full design.
