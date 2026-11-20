# ScarlettX

**ScarlettX** is an experimental, open-source high-level emulator (HLE) for **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 software, designed to execute legally obtained GDK titles end-to-end by emulating the Scarlett OS, Game Core runtime, unified memory model, `d3d12_xs`, and ray-tracing (`XboxBvhBuilder` / DXR) stack.

> **Legal notice:** ScarlettX does not include, decrypt, or distribute any proprietary Microsoft/Xbox binaries, keys, firmware, or commercial game assets. All automated tests run against synthetic x86-64 PE titles built from source in this repository.

## Architecture Overview

Because Xbox Series X|S executables are native **x86-64 PE32+** binaries targeting Microsoft's Game Development Kit (GDK), ScarlettX focuses on **high-level OS, runtime, memory, and GPU/RT emulation** rather than CPU static recompilation:

| Subsystem | Components | Status |
| :--- | :--- | :--- |
| **Package & PE Loader** | `MicrosoftGame.Config` parser (`XboxGen9`/`Scarlett` validation), PE32+ import/ordinal resolver, DLL redirection, virtual package mount points | Initial implementation + tests |
| **Kernel & GDK HLE** | `xgameruntime.dll` shim (`XGameRuntimeInitialize`, `XTaskQueue`, `XUser`, `XSystemGetDeviceType`), unified virtual/GPU VA allocator, `XStorage` async I/O | Initial implementation + tests |
| **Graphics (`d3d12_xs`)** | `d3d12_xs.dll` shim, real Windows Direct3D 12 + WARP headless backend, frame pipeline tokens, pixel readback | Initial implementation + tests |
| **Ray Tracing (DXR / BVH)** | `XboxBvhBuilder.dll` shim, BLAS/TLAS acceleration structure builder, deterministic software ray tracer (primary, shadow, reflection rays) + live D3D12 DXR tier probe | Initial implementation + tests |
| **Shader Translation** | DXBC/DXIL/Scarlett shader container inspector & IR scaffold | Scaffold + tests |

See [docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) and [docs/ROADMAP.md](docs/ROADMAP.md) for detailed design notes.

## Building & Running Tests

Requirements on Windows (x86-64):
- CMake 3.20+
- Ninja
- C++20 compiler (MinGW-w64 GCC 13+ or MSVC 2022)
- Windows D3D12 runtime + WARP (included in Windows 10/11 & Server 2022)

```bash
cmake -S . -B build -G Ninja
cmake --build build
ctest --test-dir build --output-on-failure
```

## Running the Synthetic Scarlett Test Games

```bash
# Inspect a Game Core package manifest and PE imports
./build/scarlettx inspect build/packages/hello_scarlett

# Boot the synthetic hello_scarlett title headlessly
./build/scarlettx boot build/packages/hello_scarlett --headless --frames 3

# Boot the synthetic ray-tracing showcase and dump a rendered RT frame
./build/scarlettx boot build/packages/rt_showcase --headless --frames 2 --rt-dump rt_frame.ppm
```
