# Changelog

## Unreleased (dev-probe-fix, not tested on hardware yet)

- Probe: a measurement made while X-Plane's window is minimized is discarded. Before, the native runs showed nothing, looked as fast as the bypass, and native was chosen (half the frame rate on the test laptop).
- **Moving the window to another screen now switches the path:** a screen change has to be seen twice in a row, but
  the second check reused the old answer instead of looking again, so the change was never confirmed (found in an
  outside review, reproduced with its test harness; only matters with more than one screen).
- If the probe's results stay inconsistent but the bypass was slower than native in every run, native is kept
  instead of the bypass (still not saved).
- The probe's saved result is tied to the layer version, the screen, the X-Plane image format, `vsync` and the Intel
  output, so changing any of them measures again. Older saved results are measured again once.
- README: SDR only (the bypass does not carry HDR).

## 0.9.3.2 — 2026-11-06 (updated 2026-11-07)

- **A stuck worker no longer freezes X-Plane in vkAcquireNextImageKHR:** the watchdog counts how long the worker stays
  in one stage; after 30 checks in a row (every wait the layer makes is limited to 10 s, so only a driver call that
  never returns gets there) the worker counts as lost, and an acquire that waits for an image returns
  `VK_ERROR_DEVICE_LOST`. Before, only a flush noticed a stuck worker, and an acquire could wait forever.
- The worker marks itself idle after every job, so the watchdog cannot mistake a job that returned early (for
  example the `test_fail_intel_after` branch) for a stuck worker (found in review).
- Installer: the log window's **Report** button could not reach the script's functions from its click handler
  (PowerShell closures); it now calls them through captured script blocks, and CI checks that pattern on Windows.
- vPresent checks for a lost worker right after a path switch gave up, so no job is queued on a worker that will never
  run it (found in review).
- **Regression tests for the two failure paths:** `test_hang_worker_after=N` (the worker hangs forever, like a driver
  call that never returns) and `test_fail_acquire_submit_every=N` (the acquire submit fails; the image must come back).
  Procedure in installer/docs/TESTING.md, rows 14 and 15.
- **Worker flush has a total limit:** if the worker's work does not finish within 30 s (only possible if a driver
  call never returns; every wait the layer makes is limited), the worker counts as lost. Nothing it may still use is
  freed, its thread is not joined, and X-Plane gets `VK_ERROR_DEVICE_LOST` instead of freezing.
- **vAcquire error path:** if the submit that signals X-Plane's semaphore / fence fails, the image is given back
  instead of staying taken for the rest of the session.
- README lists the known limits of the virtual swapchain (several swapchains in one present, device groups,
  `VK_GOOGLE_display_timing`, NVIDIA low-latency calls).
- **vkAcquireNextImage(2)KHR honour X-Plane's timeout:** 0 only checks (`VK_NOT_READY`), a finite timeout returns
  `VK_TIMEOUT`, `UINT64_MAX` waits as before. Nothing is signaled when no image is returned, as the spec requires.
- **A present with several swapchains** (one of them the layer's) no longer hands the layer's handle to the driver:
  the real swapchains are presented, the layer's image is released, and X-Plane is asked to use real NVIDIA
  swapchains from then on. X-Plane itself presents one swapchain; this only guards other cases.
- `measure_driver=1` also splits X-Plane's frame into waiting for the GPU (its fence polling loop), driver calls and
  its own CPU time (`[drv] frame ... =` lines); the first log line shows `measure_driver`. On the author's laptop
  X-Plane waits for the GPU for almost the whole frame, so with the bypass the GTX 1650 is the limit.
- `vkGetRefreshCycleDurationGOOGLE` reports the refresh rate of the screen X-Plane's window is on (was 60 Hz).
- Installer 0.9.3.2: anonymous reports carry a random, resettable installation ID (see README); the installer
  removes the download mark only from its own files; BENCHMARK.md no longer names an old DLL version.
- Launcher: waits for the DLL log as long as X-Plane is still starting (up to 5 minutes, was 30 s), so Steam's
  "launch options" window or a slow first start no longer shows a false "No fresh DLL log" warning.

## 0.9.3.1 — 2026-11-04 (updated 2026-11-05)

- **DXGI Present no longer has to block:** the Intel swapchain uses a frame latency waitable object; the worker waits
  for a free back buffer for at most 2 s, then treats the Intel side as failed and switches to native.
- **Every wait in the layer has a total limit:**
  - GeForce fence: 10 s (Windows normally resets a stuck GPU after about 2 s). Then the NVIDIA GPU counts as lost:
    the layer stops waiting on it, leaves memory the GPU might still write allocated instead of freeing it, and
    reports `VK_ERROR_DEVICE_LOST` to X-Plane instead of freezing.
  - Native acquire: 2 s. The frame is skipped and the layer's NVIDIA swapchain is recreated; after 3 timeouts in a
    row X-Plane gets a real NVIDIA swapchain.
  - `vkQueueWaitIdle` / `vkDeviceWaitIdle` in the layer are replaced by a fence with the same limits (NVIDIA 10 s,
    Intel 5 s); a hung device's objects are leaked rather than blocking.
  - The worker flush still waits for queued work (it uses resources the caller frees next), but every wait inside
    that work is limited; a flush over 3 s is logged as `[hang]`.
  - The limits count wait steps rather than reading the clock, so sleep / resume does not count as waiting.

## 0.9.3 — 2026-11-04

Compatibility and diagnostics rework (plan: [docs/PLAN-0.9.3.md](docs/PLAN-0.9.3.md)). Tested on an ASUS TUF F15
(GTX 1650 + UHD Graphics, Intel driver 101.1999, X-Plane 12.4.3): bypass ~44 FPS vs native ~23 FPS at KSEA, switching
both ways without a frozen picture.

- **Probe only in flight:** it waits for a loading screen (a frame gap over 2 s) plus 30 s and ignores menu frame
  rates (under 6 ms per frame); a loading pause during the measurement restarts it, and if the two runs of a path
  differ by more than 30% it measures again (twice), then keeps the bypass without saving a result. Found in testing:
  starting in X-Plane's main menu made it pick native.
- **No leftover console windows:** Windows Terminal (default on Windows 11) ignores PowerShell's hidden window style,
  so an empty PowerShell window stayed open while the launcher waited for X-Plane. The launcher, the shortcuts and
  the installer now run through a headless console host (Windows 10 1903+); the BAT window closes right away.
- **Notice before installing:** the installer says that anonymous reports are on by default (and how to opt out),
  and that the one-time native/bypass measurement briefly halves the frame rate and stutters at each switch.

- **Always a virtual swapchain** when the layer is active; the output path (bypass through the Intel GPU, or native
  through a layer-owned NVIDIA swapchain) is switched inside the layer. No reliance on X-Plane recreating its
  swapchain (`VK_SUBOPTIMAL_KHR` is not guaranteed to cause that). `OUT_OF_DATE` remains only as a last resort.
- **Screen changes:** the window's screen is checked every second; moving it to a screen driven by another GPU
  switches to native and back (seen twice, 2 s cooldown).
- **Measured path selection** (`path=auto`, `probe=1`): native / bypass / native / bypass, 300 frames each after the
  frame rate is stable; bypass is kept only with >= 5% lower P50 and <= 10% higher P95. Cached per GPU, driver and
  screen in `xpob_path_cache.txt`; re-measured if P95 gets > 30% worse for 30 s (at most every 10 minutes).
- **Intel device chosen by LUID** (the GPU that drives X-Plane's screen), not the first Intel device.
- **Without `VK_EXT_external_memory_host`:** half-import or CPU copy instead of disabling the bypass; the alignment
  is checked instead of assumed.
- **No spare graphics queue:** a transfer-only queue is used (images in concurrent sharing mode).
- **Formats and sizes:** non-BGRA8 SDR formats are converted with a GeForce blit; size differences are scaled with an
  Intel blit; X-Plane is offered only SDR formats the bypass can handle; HDR color spaces use the native path.
- **Diagnostics:** `[diag]` lines (GPUs, LUIDs, extensions, queue families, DXGI adapters and screens, swapchain
  request, Intel surface formats, implicit layers, overlay DLLs, chosen path and reason). Paths in the log hide the
  user name. Every 300 frames: frame-time P50/P95/P99/max/over 50 ms and per-path GPU copy (timestamps), CPU copy,
  fence wait, acquire, present and the layer's own time on X-Plane's thread.
- **INI:** `path`, `probe` and test switches `test_force_blit`, `test_force_cpu_copy`, `test_no_extra_queue`,
  `test_fail_intel_after`.
- **Installer 0.9.3:** Global mode is an advanced option with an anti-cheat confirmation; detects running from inside
  the ZIP and restricted PowerShell; more detail in `installer-error.log`; `fallback` folder installs without
  PowerShell.
- **`measure_driver=1`** (diagnostics): every Vulkan device call X-Plane makes is timed (wrappers generated from the
  Vulkan headers by `tools/gen_measure_hooks.py`); every 300 frames a `[drv]` line shows how many ms per frame
  X-Plane's thread spends in the driver for draws, binds, render passes, copies/barriers, descriptors, submits, waits,
  mapping and pipeline creation, plus calls and draw calls per frame.
- **Intel output through DXGI flip mode (D3D12)**, new default (`intel_output=auto`): the Intel GPU shows the frame
  through a flip-model swapchain, reading the transfer block in place (`OpenExistingHeapFromAddress`) or through an
  upload buffer. Both paths now use flip mode, so the layer can switch native <-> bypass again and the probe is back
  on by default. `intel_output=vulkan` keeps the 0.9.2-style Intel Vulkan output (no return after native).
- **No return to the bypass after native output** (found on the test laptop): NVIDIA's native output puts the
  window into DXGI flip mode, and an older Intel driver's output is then not shown any more, so the picture froze
  while X-Plane kept running. The layer now stays native for the rest of the session once native output was shown;
  therefore the native/bypass probe is **off by default** (`probe=0`) and needs `test_bypass_after_native=1/2`.
  Reinstalling keeps `xpob_path_cache.txt`. Tested: a fresh Intel surface (`test_bypass_after_native=2`) does not
  help with Intel 101.1999; a flip-model (DXGI) Intel output is planned so both directions can switch.
- **No freeze on a stuck Intel driver:** Intel fence and acquire waits have a time limit (5 s / 2 s); after that the
  Intel side counts as failed, the layer never waits on it again and switches to the native path. A watchdog writes
  `[hang]` lines (where X-Plane's thread and the worker are) when no frame is shown for 3 s. The trial host-import
  block is kept until teardown (freeing it right away made an old Intel driver report a lost device).
- **Steam installs:** Steam restarted X-Plane when the launcher started it, and the restarted process did not load the
  "X-Plane only" layer. The launcher now puts a `steam_appid.txt` (the app id from Steam's appmanifest) next to
  `X-Plane.exe` if none is there, so X-Plane starts directly (Steam must be running); uninstall removes only a file it
  created. If Steam still restarts X-Plane, the launcher says so instead of a generic warning.
- **Anonymous compatibility reports** sent by the launcher after X-Plane closes: on by default, visible in the
  installer, can be switched off; only whitelisted fields, no paths or identifiers. Report server in `server/`
  (Cloudflare Worker, stores no IP). Public aggregated results: https://xpob-reports.jjjay0826.workers.dev/v1/summary

## 0.9.1.1-beta — 2026-11-03

- **New default install: X-Plane only.** `install.bat` now asks how to install. Option 1 (recommended) registers the
  layer with `enable_environment`, so the Vulkan loader only loads it when `XP_OPTIMUS_BYPASS_ENABLE=1` is set; start
  X-Plane with the new `start_xplane.bat`. Other Vulkan programs (including games with anti-cheat) no longer load the
  DLL at all. Option 2 keeps the 0.9.1 behaviour (all Vulkan programs, `xp_optimus_bypass_global.json`).
- `install.bat` and `uninstall.bat` remove every earlier XP Optimus Bypass registry entry, also from other folders.
- **Falls back instead of crashing:** if the Intel side fails during a flight (device or surface lost, or 30
  failed frames in a row), the layer tells X-Plane its swapchain is out of date and gives it NVIDIA's normal swapchain
  for the rest of the session. FPS drops back to the normal level, but X-Plane keeps running. Previously such an error
  was passed to X-Plane as "device lost", which would most likely end the session.
- The log shows whether X-Plane was started with `XP_OPTIMUS_BYPASS_ENABLE=1`, the Windows build, the X-Plane.exe
  file version and the NVIDIA and Intel driver versions.
- No change to the bypass itself when everything works.
- Release zips are now built by GitHub Actions, with SHA-256 in the release notes and a build-provenance
  attestation. The build is reproducible (byte-identical DLL from the same source and toolchain).

## 0.9.1-beta — 2026-11-02

- **Clean shutdown:** when X-Plane destroys its Vulkan device, the worker thread is stopped and joined and the Intel
  Vulkan device, surface and instance are released. Previously they were left running until the process ended, which
  could crash X-Plane on exit if the Vulkan loader unloaded the layer first. (X-Plane 12.4.3 seems to quit without
  destroying its Vulkan device, so the log line `bypass shut down cleanly` may not appear; that is harmless.)
- **FPS logging when the bypass is not active** (`mode=off`, or `mode=auto` on a system where it does not apply), so
  off/on comparisons use the same measurement. Log lines start with `FPS ... | bypass NOT active`.
- New [BENCHMARK.md](BENCHMARK.md): a fixed test procedure for comparable results.
- README: note that the DLL is not code-signed, how to check the SHA-256 checksum, the anti-cheat caveat, and possible
  future work.
- Release zip now contains `SHA256SUMS.txt`.
- GitHub issue templates for benchmark results and bug reports.

## 0.9.0-beta — 2026-11-01

- First public beta: presents X-Plane 12 frames through the Intel iGPU instead of NVIDIA's Optimus present path.
- `mode=auto|on|off`, `vsync`, `stats` settings; automatic detection of NVIDIA render GPU + Intel-driven display.
- Handles window resize/minimize by recreating the Intel swapchain internally.
- Per-300-frame log with FPS, average and worst-frame timings for each stage.
