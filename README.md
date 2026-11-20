# ScarlettX

**ScarlettX** is an experimental, open-source high-level emulator (HLE) for **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 software, designed to execute legally obtained GDK titles end-to-end by emulating the Scarlett OS, Game Core runtime, unified memory model, `d3d12_xs`, and ray-tracing (`XboxBvhBuilder` / DXR) stack.

> **Legal notice:** ScarlettX does not include, decrypt, or distribute any proprietary Microsoft/Xbox binaries, keys, firmware, or commercial game assets. All automated tests run against synthetic x86-64 PE titles built from source in this repository.

<!-- SCARLETTX_STATUS_START -->
## Status

![ScarlettX Status](docs/status.svg)

**Overall Milestone Progress:** `81.2%` (177/218 tracked units) · **Exported HLE Symbols:** `164` · **Automated CTest Suite:** `13/13` passing

| Subsystem | Category | Progress | Coverage | Details |
| :--- | :--- | :---: | :---: | :--- |
| **PE64 Loader & Auto-Stubs** | `Core` | `6/6` | `100%` | PE32+ mapping, DIR64/HIGHLOW relocs, named/ordinal IAT, API-set redirection, x64 auto-stub thunks |
| **XTaskQueue & XAsync** | `xgameruntime` | `15/24` | `62%` | 11 XTaskQueue + 4 XAsync exports (manual/threadpool/delayed/monitors/cancel) |
| **XGameSave (Persistent)** | `xgameruntime` | `20/22` | `91%` | 20 exports; disk-backed containers, sync/async blob R/W, quota, delete |
| **XUser & Identity** | `xgameruntime` | `13/24` | `54%` | 13 exports; synthetic Xbox Live profile, XUID, gamertag components, privileges |
| **XSystem & Runtime** | `xgameruntime` | `12/18` | `67%` | 12 exports; Scarlett Anaconda device info, runtime/analytics, handle validation |
| **XMem & Threading** | `xgameruntime` | `11/16` | `69%` | 11 exports; default/custom alloc hooks, VirtualAlloc/Free, time-sensitive thread state |
| **d3d12_xs → PC D3D12** | `Graphics` | `28/32` | `88%` | 26 exports; D3D12CreateDeviceX → real ID3D12Device, root-sig forwarding, PresentXEx/frame stats, Suspend/Resume, ResourceAllocationInfo, WARP backend |
| **DXR / XboxBvhBuilder** | `Graphics` | `12/14` | `86%` | 8 BVH exports + DXR AS prebuild/build/destroy, BLAS update, single-ray trace, deterministic software BVH fallback (hardware DXR requires capable GPU) |
| **DirectStorage & XVD** | `Storage` | `23/24` | `96%` | 22 DStorage exports + unencrypted XVD parser/PackageVfs mounting; async worker queues, fence/status arrays, cancellation, synthetic LZ-RLE test codec |
| **Audio (XAudio2/XMA2)** | `Media` | `22/22` | `100%` | 22 exports; mastering/submix/source voices, headless PCM mixer, output matrix/volume/pitch, WAV writer, synthetic XMA2 DPCM decoder |
| **Input (GameInput)** | `Input` | `15/16` | `94%` | 15 exports; virtual Xbox Series controller & keyboard, reading history (Current/Next/Previous), state injection, 4-motor impulse-trigger rumble |

_Regenerate with `python tools/generate_status.py` after adding new HLE APIs or subsystems._
<!-- SCARLETTX_STATUS_END -->

## Architecture Overview

Because Xbox Series X|S executables are native **x86-64 PE32+** binaries targeting Microsoft's Game Development Kit (GDK), ScarlettX focuses on **high-level OS, runtime, memory, and GPU/RT emulation** rather than CPU static recompilation:

| Subsystem | Components | Status |
| :--- | :--- | :--- |
| **Package & PE64 Loader** | `MicrosoftGame.Config` parser (`XboxGen9`/`Scarlett`), in-process PE64 mapper (`DIR64`/`HIGHLOW` relocations, page protections), named/ordinal IAT & API-set resolution, x64 auto-stub thunk generator | Milestone 1 complete + tests |
| **GDK Runtime (`xgameruntime.dll`)** | Real C ABI for `XGameRuntime*`, `XTaskQueue*`, `XAsync*`, `XUser*`, persistent disk-backed `XGameSave*`, `XSystem*`, `XMem*` hooks & virtual memory, `XThread*`, and `XStorage` | Milestone 2 complete + tests |
| **Graphics (`d3d12_xs`)** | `d3d12_xs.dll` → PC D3D12 translation (`D3D12CreateDeviceX` returning real `ID3D12Device`, root-signature forwarding, `PresentXEx` + queue fence sync, frame interval/stats, `SuspendX`/`ResumeX`, `GetResourceAllocationInfo`, headless WARP backend) | Milestone 3 complete + tests |
| **Ray Tracing (DXR / BVH)** | `d3d12_xs` DXR acceleration-structure prebuild/build/destroy + `XboxBvhBuilder.dll` (prebuild, BLAS build/update, TLAS, stats, single-ray trace, deterministic software BVH fallback; hardware DXR requires a DXR-capable GPU) | Milestone 3 complete + tests |
| **DirectStorage & XVD (`dstorage.dll`)** | Async worker-thread I/O queues, file/memory/XVD reads, fence callbacks, status arrays, cancellation by tag, synthetic LZ-RLE test codec, unencrypted XVD container parser + `PackageVfs::MountXvd` | Milestone 4 complete + tests |
| **Audio (`xaudio2_9.dll` / XMA2)** | XAudio2 engine + mastering/submix/source voices, deterministic headless PCM mixer, volume/pitch/output-matrix panning, WAV export, synthetic XMA2 packet decoder | Milestone 4 complete + tests |
| **Input (`GameInput.dll`)** | Virtual Xbox Series controller & keyboard enumeration, reading history (`Current`/`Next`/`Previous`), deterministic input injection, 4-motor impulse-trigger rumble | Milestone 4 complete + tests |
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
