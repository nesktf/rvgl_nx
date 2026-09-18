# RVGL Nintendo Switch Port (`rvgl_nx`) - Development Summary
*Summary of changes, architectural additions, and bug fixes implemented after commit `804116`.*

---

## 1. Executive Summary

Following commit `804116`, the `rvgl_nx` project was transitioned from an initial loader scaffold into a fully working, stable, and performant Nintendo Switch port of RVGL (Re-Volt GL). Key achievements include:
- A custom 64-bit ARM ELF dynamic loader supporting multi-module dependency resolution.
- A comprehensive Bionic-to-Horizon/newlib libc and POSIX emulation shim.
- Resilient networking and socket handling enabling LAN multiplayer and game hosting.
- Full Nintendo Switch Software Keyboard (`swkbd`) integration with direct memory text injection.
- Complete lifecycle management and memory cleanup (`so_unload`), eliminating exit crashes and Atmosphere fatal errors when returning to `hbmenu`.

---

## 2. ELF Loader & Memory Architecture (`so_util.c`, `so_util.h`, `main.c`)

### 64-bit ELF Loading & Multi-Module Dynamic Linking
- **ELF64 Parser & Relocations**: Implemented support for parsing 64-bit ELF shared objects (`.so`) compiled for Android `aarch64`. Added relocation handlers for `R_AARCH64_RELATIVE`, `R_AARCH64_GLOB_DAT`, `R_AARCH64_JUMP_SLOT`, and `R_AARCH64_ABS64`.
- **Multi-Module Dependency Loading**: Orchestrated sequential loading and cross-module symbol resolution for:
  1. `libsndfile.so`
  2. `libunistring.so`
  3. `libmain.so` (RVGL core binary)
- **Symbol Resolution Hierarchy**: Symbols are dynamically resolved first across the export tables of loaded dependencies (`so_symbol`), then falling back to host imports (`imports.c`).

### Horizon OS Heap Separation
- **Dual Heap Layout (`__libnx_initheap`)**: Horizon OS restricts process heap resizing once pages are marked executable. Overrode `__libnx_initheap()` to split the available address space into:
  - **Newlib Heap (`fake_heap`)**: Dedicated to standard library allocations (`malloc`, Switch OS service buffers).
  - **SO Code & Data Space (`heap_so_base`)**: Aligned to page boundaries, reserved exclusively for `.so` segment loading.
- **Code Memory Mapping**: Executable segments are mapped into unreserved virtual memory via `svcMapProcessCodeMemory` and protected with `svcSetProcessMemoryPermission(Perm_Rx)`.

### Clean Process Teardown (`so_unload`)
- **Root Cause of Exit Crash**: When returning to `hbmenu`, Horizon's `svcSetHeapSize()` failed because process heap pages were still mapped as code memory (`svcMapProcessCodeMemory`), causing an Atmosphere kernel panic / fatal error (`2001-0106`).
- **Unloading Routine**: Implemented `so_unload()`:
  1. Remaps all RX segments back to `Perm_Rw`.
  2. Unmaps executable code memory with `svcUnmapProcessCodeMemory()`.
  3. Releases virtual memory reservations with `virtmemRemoveReservation()`.
- Added module teardown calls in `main()` before return, ensuring a clean exit back to `hbmenu`.

---

## 3. Libc & POSIX Compatibility Layer (`libc_shim.c`, `libc_shim.h`, `imports.c`)

Because the Android `.so` expects a Linux/Bionic runtime environment, an extensive translation layer was implemented:

### Filesystem & Path Redirection
- **Virtual Path Remapping**: Translated Android-specific paths (`/data/data/com.rvgl...`) to Switch SD card locations:
  - Assets: `/switch/rvgl/assets/`
  - User Profiles / Config: `/switch/rvgl/`
- **Case-Insensitive File Lookup**: Case sensitivity discrepancies between PC/Linux assets and FAT32/exFAT SD cards were resolved with case-insensitive fallback resolution in `fopen()`, `open()`, `stat()`, `access()`, and `opendir()`.
- **Stream Redirection Hook (`freopen`)**: RVGL internally uses `freopen()` on `stdout` and temporary streams for parsing text files (car parameters, track configs). Shimmed `freopen()` to correctly reopen target files without breaking active console output.

### Networking & Sockets (LAN Play & Server Hosting)
- **Flag Translation**: Translated BSD vs Linux socket options and non-blocking flags (`O_NONBLOCK`, `MSG_DONTWAIT`).
- **Broadcast & Hosting Fixes**:
  - Enabled Switch BSD socket flags for broadcast (`SO_BROADCAST`), address reuse (`SO_REUSEADDR`), and multicast.
  - Resolved socket binding and address resolution discrepancies in `getaddrinfo` and `bind`, fixing both LAN game hosting and client connection timeouts.

### Multithreading & Synchronization
- **Pthreads Wrapper**: Mapped POSIX threads (`pthread_create`, `pthread_join`, `pthread_mutex_*`, `pthread_cond_*`) to libnx threading primitives.
- **Stack Size Guarantee**: Configured a default 2MB stack for spawned threads to accommodate RVGL's sound decoding and asset loading routines without stack overflow.

---

## 4. Input System & Software Keyboard (`hooks/game.c`)

### Native Software Keyboard (`swkbd`)
- Integrated Nintendo Switch native Software Keyboard (`swkbdCreate`, `swkbdConfigMakePresetDefault`, `swkbdShow`).
- **On-Demand User Triggering**: Prevented the keyboard from opening automatically or repeatedly in non-text contexts. The keyboard opens strictly when requested by the user.
- **Physical Button Mapping**: Configured physical **X** and **Y** face buttons (mapped via `SDL_CONTROLLER_BUTTON_X` / `SDL_CONTROLLER_BUTTON_Y`) to bring up `swkbd` whenever editing a text field.

### Direct Memory Text Injection
- **RVGL Input Architecture**: RVGL's text input screens (`DrawHostComputer`, `DrawEnterName`) do not poll standard SDL text input events; instead, they inspect raw keyboard scancode tables.
- **Buffer Injection (`apply_text_input`)**: Resolved input non-responsiveness by writing user input directly into RVGL's target memory buffers upon keyboard confirmation:
  - **Host IP Connection Field**: Written to `settings + 0x39c`.
  - **Profile / Player Name Field**: Written to `titlescreen_data + 40`.

### Controller Button Leak & Freeze Elimination
- Confirming text in `swkbd` using the controller (A button) caused the (A) press to leak into the game event queue immediately upon closing.
- In the Host Connection menu, this accidental press instantly triggered "Connect", causing an unexpected 5-second network timeout freeze.
- Added an input draining loop (`SDL_PumpEvents` + `SDL_FlushEvents`) for 250ms following `swkbdClose()`, cleanly discarding trailing confirmation presses and maintaining smooth 60 FPS rendering.

---

## 5. Graphics, Audio & Platform Integration

### Resolution & Docking Management (`main.c`, `hooks/opengl.c`)
- **Docked vs Handheld**:
  - Handheld mode: 1280x720.
  - Docked mode: 1920x1080.
- **Config Synchronization**: Added `read_ini_resolution()` to parse user-defined settings in `profiles/rvgl.ini` while dynamically adapting to console dock status.
- **OpenGL Pipeline**: Integrated with the Switch Mesa/Nouveau OpenGL driver with support for frame pacing and FPS monitoring.

### Audio Teardown (`hooks/openal.c`)
- Configured OpenAL-Soft output using the Switch audio renderer.
- Added `deinit_openal()` to release OpenAL devices and contexts gracefully during shutdown.

### Crash Diagnostics (`main.c`, `util.c`)
- **Atmosphere Crash Report Dumper**: Implemented `dump_latest_crash_report()` to read `/atmosphere/crash_reports/` on startup and output the stack trace directly to the nxlink log.
- **nxlink Logging**: Remote network debugging with automatic socket initialization and stdout flushing.

---

## 6. Modified and Created Files

| File | Status | Description |
|---|---|---|
| `source/libc_shim.c` | **New** | Complete Bionic/POSIX shim library (I/O, memory, threads, math, sockets). |
| `source/libc_shim.h` | **New** | Header definitions and type mappings for libc shims. |
| `source/so_util.c` | **Modified** | 64-bit ELF loader, relocation processing, `so_unload` code unmapping. |
| `source/so_util.h` | **Modified** | Module definitions, function prototypes for dynamic linker and unloader. |
| `source/main.c` | **Modified** | Multi-module loading, heap splitting, resolution management, cleanup sequence, crash dumper. |
| `source/imports.c` | **Modified** | Symbol redirect table mapping 400+ Android symbols to native Switch implementations. |
| `source/hooks/game.c` | **Modified** | Native `swkbd` integration, direct memory text injection, event flush loop. |
| `source/hooks/openal.c` | **Modified** | OpenAL-Soft backend integration and graceful deinitialization. |
| `source/hooks/opengl.c` | **Modified** | OpenGL hooks and viewport setup. |
| `source/util.c` | **Modified** | nxlink network logging and app exit handler. |
| `Makefile` | **Modified** | Build configuration, includes, compiler flags, and link dependencies. |

---

## 7. Multiplayer Stability & Clean Exit Teardown

### Issue 1: Socket Initialization Regression
- **Root Cause**: `initNxLink()` and socket initialization routines in `source/util.c` were previously wrapped under `#ifdef DEBUG_LOG`. When `DEBUG_LOG` was commented out in `config.h`, `socketInitialize()` was never called on startup, causing LAN multiplayer hosting and joining to do nothing.
- **Fix**:
  - Decoupled network and socket initialization from debug logging by introducing `init_network()` and `deinit_network()` in `source/util.c` with a fallback to `socketInitializeDefault()`.
  - Added safeguards against double-conversion in `sockaddr_bionic_to_nx` and `sockaddr_nx_to_bionic` in `source/libc_shim.c`.

### Issue 2: Audio Double-Free on Normal Exit
- **Root Cause**: RVGL's `ReleaseAudio()` teardown function cleans up its own OpenAL audio subsystem on exit, calling `alcDestroyContext` and `alcCloseDevice`. The wrapper's subsequent call to `deinit_openal()` in `main.c` attempted to destroy the already-freed context and close the closed device pointer, causing memory corruption.
- **Fix**: Installed `alcDestroyContextHook` and `alcCloseDeviceHook` in `source/imports.c` to clear the stored `al_ctx` and `al_dev` pointers when RVGL destroys them, ensuring `deinit_openal()` is safe and idempotent.

### Issue 3: Exit Crash (2001-0106 / InvalidCurrentMemory) After Multiplayer
- **Root Cause**: When hosting or joining a multiplayer session, RVGL's `InitNetwork(bool)` spawned a detached background thread named `"IP Thread"` (`SDL_CreateThread`) that queried an external WAN IP service (`api.ipify.org` / `fetch_ip`) and looped in `SDL_Delay(10000)`. When exiting the game, this detached thread remained alive with its 2MB stack allocated on the heap. When `main()` exited to the Homebrew Loader (`hbl`), `hbl` called `svcSetHeapSize()` to reset the heap; Horizon OS rejected the call with `0xD401` (`ResultInvalidCurrentMemory`) due to active thread stack pages inside the heap, crashing `hbl`.
- **Fix**:
  - In `source/hooks/game.c`, pre-populated both `net_local_ip_string` and `net_public_ip_string` with the Switch's IP in `set_switch_local_ip()` and set `net_have_public_ip = 1`.
  - Patched `InitNetworkb` at runtime to NOP out the branch initiating the `"IP Thread"` creation, along with the `SDL_CreateThread` and `SDL_DetachThread` calls.
  - Added an interception guard in `SDL_CreateThread_hook` (`source/imports.c`) to block any `"IP Thread"` creation.
  - In `source/main.c`, explicitly called `ReleaseNetwork()` on exit to disconnect active ENet peers and close open sockets before module unmapping and network deinitialization.

