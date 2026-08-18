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

## Linux (CI)

The `linux-clang` preset is ready for CI. The GitHub Actions workflow lands in
a later subtask (T2).

## What's next

- **T2:** Linux CI + AddressSanitizer preset
- **T3+:** Domain types (`Result`, `Money`, IDs) and ISO 8583 codec

See [docs/superpowers/specs/2026-08-15-aegis-design.md](docs/superpowers/specs/2026-08-15-aegis-design.md)
for the full design.
