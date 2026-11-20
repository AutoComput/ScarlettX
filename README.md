# ScarlettX

**ScarlettX** is an experimental, open-source high-level emulator (HLE) for **Xbox Series X|S (Scarlett / Gen9 Game Core)** x86-64 software, designed to execute legally obtained GDK titles end-to-end by emulating the Scarlett OS, Game Core runtime, unified memory model, `d3d12_xs`, and ray-tracing (`XboxBvhBuilder` / DXR) stack.

> **Legal notice:** ScarlettX does not include, decrypt, or distribute any proprietary Microsoft/Xbox binaries, keys, firmware, or commercial game assets. All automated tests run against synthetic x86-64 PE titles built from source in this repository.

<!-- SCARLETTX_STATUS_START -->
## Status

![ScarlettX Status](docs/status.svg)

**Status:** `76.4%` (252 of 330 tracked functions) · **Exports:** `240` · **Tests:** `16/16` passing · **Target:** `Xbox Series X|S (Gen9)`

| Subsystem | Category | Progress | Coverage | Details |
| :--- | :--- | :---: | :---: | :--- |
| **PE64 Loader & Auto-Stubs** | `Core` | `6 of 6` | `100%` | PE32+ mapping, DIR64/HIGHLOW relocs, named/ordinal IAT, API-set redirection, x64 auto-stub thunks |
| **DirectStorage & XVD** | `Storage` | `23 of 24` | `96%` | 22 DStorage exports + unencrypted XVD parser/PackageVfs mounting; async worker queues, fence/status arrays, cancellation, synthetic LZ-RLE test codec |
| **d3d12_xs → PC D3D12 & PSOs** | `Graphics` | `44 of 46` | `96%` | 43 exports + DXIL/DXBC container parser & Xbox-chunk stripper; root signatures, descriptor heaps, graphics/compute/software-DXR PSOs, pipeline cache, WARP backend |
| **XSystem & Runtime** | `xgameruntime` | `17 of 18` | `94%` | 17 exports; Scarlett Anaconda device info, runtime/analytics, handle validation |
| **Input (GameInput)** | `Input` | `15 of 16` | `94%` | 15 exports; virtual Xbox Series controller & keyboard, reading history (Current/Next/Previous), state injection, 4-motor impulse-trigger rumble |
| **XGameSave (persistent)** | `xgameruntime` | `20 of 22` | `91%` | 20 exports; disk-backed containers, sync/async blob R/W, quota, delete |
| **DXR / XboxBvhBuilder** | `Graphics` | `12 of 14` | `86%` | 8 BVH exports + DXR AS prebuild/build/destroy, BLAS update, single-ray trace, deterministic software BVH fallback (hardware DXR requires capable GPU) |
| **Networking (HTTP / XCurl)** | `Networking` | `31 of 40` | `78%` | 19 libHttpClient.GDK + 12 XCurl exports; deterministic offline mock registry, async callbacks, headers, fallback responses |
| **Audio (XAudio2 / AcpHal)** | `Media` | `23 of 32` | `72%` | 23 exports (XAudio2_9 ordinal #1 + AcpHal.dll APU/SHAPE shims); headless PCM mixer, WAV writer, fail-closed real-title COM guard, synthetic XMA2 test decoder |
| **XMem & Threading** | `xgameruntime` | `11 of 16` | `69%` | 11 exports; default/custom alloc hooks, VirtualAlloc/Free, time-sensitive thread state |
| **Middleware (Bink2 / FSR)** | `Middleware` | `22 of 34` | `65%` | 12 Bink2Scarlett + 10 amd_fidelityfx_loader/upscaler_xbox exports; deterministic offline HLE shims (synthetic RGBA/BGRA frames, bilinear upscale & hash) |
| **XTaskQueue & XAsync** | `xgameruntime` | `15 of 24` | `62%` | 11 XTaskQueue + 4 XAsync exports (manual/threadpool/delayed/monitors/cancel) |
| **XUser & Identity** | `xgameruntime` | `13 of 24` | `54%` | 13 exports; synthetic Xbox Live profile, XUID, gamertag components, privileges |
| **Retail Crypto & Native Codecs** | `Platform` | `0 of 14` | `0%` | Encrypted retail XVD, production GDeflate/XMA2/Bink codecs, RDNA2 XS2 ISA lifting, hardware DXR, Vulkan |

> **Not started:** encrypted retail XVD decryption, production GDeflate/XMA2/Bink decoding, precompiled RDNA2 `XS2` ISA lifting, hardware DXR (software BVH fallback is verified on WARP), and Vulkan backend. Passing synthetic tests do not imply commercial retail title execution.

_Regenerate with `python tools/generate_status.py` after adding new HLE APIs or subsystems._
<!-- SCARLETTX_STATUS_END -->

## Architecture Overview

Because Xbox Series X|S executables are native **x86-64 PE32+** binaries targeting Microsoft's Game Development Kit (GDK), ScarlettX focuses on **high-level OS, runtime, memory, and GPU/RT emulation** rather than CPU static recompilation:

| Subsystem | Components | Status |
| :--- | :--- | :--- |
| **Package & PE64 Loader** | `MicrosoftGame.Config` parser (`XboxGen9`/`Scarlett`), in-process PE64 mapper (`DIR64`/`HIGHLOW` relocations, page protections), named/ordinal IAT & API-set resolution (`api-ms-win-core-synch-l1-2-*`, `rtlsupport`, `psm-appnotify`), dynamic `LoadLibrary` shim redirection, x64 auto-stub thunk generator | Phase 1 updated + tests |
| **GDK Runtime (`xgameruntime.dll`)** | C ABI for `XGameRuntime*`, `XTaskQueue*`, `XAsync*`, `XUser*`, persistent disk-backed `XGameSave*`, `XSystem*`, `XMem*` hooks & virtual memory, `XThread*`, `XStorage`, plus real-target `InitializeApiImpl`/`InitializeApiImplEx(build, rev, flags)`, `UninitializeApiImpl`, `QueryApiImpl(api_id, riid, out_impl)` diagnostics, and `XErrorReport` | Phase 1 updated + tests |
| **Graphics (`d3d12_xs` / `xg_xs`)** | `d3d12_xs.dll` (`D3D12XboxCreateDevice`, `D3D12XboxSetProcessDebugFlags`, `D3D12SerializeRootSignature`, `D3D12XboxCreateSyntheticDevice`, fail-closed COM boundary for real titles until Xbox `ID3D12Device` vtable wrapper is implemented) + `xg_xs.dll` (`XGCreateTexture2DComputer`, `XGComputeOptimalSwizzleMode`) | Phase 1 cleanup + tests |
| **Ray Tracing (DXR / BVH)** | Synthetic `d3d12_xs` DXR acceleration-structure prebuild/build/destroy + `XboxBvhBuilder.dll` (synthetic signatures + deterministic software BVH fallback; hardware DXR requires a DXR-capable GPU) | Synthetic scaffold + tests |
| **DirectStorage & XVD (`dstorage_xs.dll`)** | Renamed to Xbox `dstorage_xs.dll`; async worker-thread I/O queues, file/memory/XVD reads, fence callbacks, status arrays, cancellation by tag, synthetic LZ-RLE test codec, unencrypted XVD container parser + `PackageVfs::MountXvd` | Phase 1 cleanup + tests |
| **Audio (`xaudio2_9.dll` / `AcpHal.dll`)** | `XAudio2_9.dll` with verified ordinal `#1` (`XAudio2Create`, failing closed for real titles until Xbox COM wrapper exists) + `AcpHal.dll` (`ApuAlloc`/`ApuFree`/`ApuMapVirtualAddress`/`ApuIsVirtualAddressValid`/`AcpHalCreate`/`AcpHalAllocateShapeContexts`/`AcpHalReleaseShapeContexts`) + headless PCM mixer & synthetic XMA2 test decoder | Phase 1 updated + tests |
| **Input (`GameInput.dll`)** | `GameInput.dll` with verified ordinal `#1` (`GameInputCreate`), virtual Xbox Series controller & keyboard enumeration, reading history (`Current`/`Next`/`Previous`), deterministic input injection, 4-motor impulse-trigger rumble | Phase 1 updated + tests |
| **Shader & PSO Pipeline** | DXIL/DXBC container parser (stage, shader model, I/O params, RT exports, embedded root sig, content hash), Xbox-private chunk stripping (`XBOX`/`XS2M`), explicit unsupported-format error on precompiled RDNA2 `XS2`, D3D12 root signatures, descriptor heaps, graphics/compute/software-DXR PSOs, pipeline cache stats, draw/dispatch/RT command execution | Synthetic scaffold + tests |
| **Middleware (`Bink2` / `FidelityFX`)** | `Bink2Scarlett.dll` (open/decode/advance/pause/seek/RGBA/BGRA/summary), `amd_fidelityfx_loader_xbox.dll` & `amd_fidelityfx_upscaler_xbox.dll` (context lifecycle, quality ratios, render resolution, jitter, deterministic bilinear upscale & frame hash) | Synthetic scaffold + tests |
| **Networking (`libHttpClient` / `XCurl`)** | `libHttpClient.GDK.dll` (init, mock registry, request setup, async callbacks, offline fallback), `XCurl.dll` (easy handle lifecycle, `curl_easy_setopt`, write callbacks, `curl_easy_getinfo`, slist headers, mock/offline responses) | Synthetic scaffold + tests |
| **End-to-End Integration** | `scarlett_arena` synthetic Gen9 game booting via in-process PE64 mapper (with isolated guest `atexit` cleanup) and child process across `xgameruntime`, `dstorage_xs`/XVD, `GameInput`, `d3d12_xs` + software BVH RT, `XAudio2`, and `XGameSave` | Synthetic E2E + tests |

### Phase 1 Real-Title Bring-Up (Verified Observations)

Testing against an unencrypted commercial Xbox Series X|S (`Scarlett`) Unity IL2CPP title (kept strictly outside the repository) established:

- **Static inspection & image mapping:** PE64 image maps with 405 imported symbols and 3 remaining auto-stubs; ScarlettX's `d3d12_xs.dll`, `xg_xs.dll`, and `AcpHal.dll` resolve all static Xbox imports.
- **Controlled boot trace:** Redirecting `xgameruntime.dll` loads ScarlettX's shim and invokes `InitializeApiImplEx(build=19041, rev=10002, flags=0x0)` followed by `QueryApiImpl(api={073B7DCB-1FCF-4030-94BE-E3C9EB623428}, iid={073B7DCB-1FCF-4030-94BE-E3C9EB623428})`, which currently returns `E_NOINTERFACE` and causes clean exit code `1`.
- **Current classification:** **Static inspection + image mapping + early startup trace** (not yet partial boot or gameplay).

### Current Limitations

- **Xbox COM vtable wrappers:** Real-title `D3D12XboxCreateDevice` and `XAudio2Create` fail closed (`E_NOTIMPL`) rather than exposing raw PC COM pointers; full Xbox `ID3D12Device`/`IXAudio2` vtable wrappers and `QueryApiImpl` GDK COM interfaces are next.
- **Retail packages:** Encrypted retail `.xvc` / XVD packages are not supported; only unencrypted loose packages and unencrypted XVDs are mounted.
- **Shaders & Ray Tracing:** Precompiled RDNA2 `XS2` ISA lifting is not implemented (returns `UnsupportedFormat`); hardware DXR requires a DXR-capable GPU (CI/WARP runs the verified software BVH fallback).
- **Codecs & Middleware:** `Bink2Scarlett`, `amd_fidelityfx_*_xbox`, `dstorage_xs` decompression, and `XMA2` use deterministic offline/synthetic HLE shims rather than production Bink, GPU FSR, GDeflate, or transform XMA2 codecs.
- **Networking & Graphics Backend:** `libHttpClient.GDK` and `XCurl` run offline with mock/fallback responses; graphics currently target PC D3D12 (no native Vulkan backend yet). Passing synthetic tests do not imply commercial retail title execution.

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

# Run the full-stack end-to-end synthetic game demo (PE64 + XVD/DirectStorage + GameInput + D3D12/BVH RT + XAudio2/XMA2 + XGameSave)
./build/test_scarlett_arena_e2e
```
