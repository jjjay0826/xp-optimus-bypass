# Changelog

## 0.9.3-beta — 2026-11-04

Compatibility and diagnostics rework (plan: [docs/PLAN-0.9.3.md](docs/PLAN-0.9.3.md)). Not yet tested on Windows.

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
- **Anonymous compatibility reports** sent by the launcher after X-Plane closes: on by default, visible in the
  installer, can be switched off; only whitelisted fields, no paths or identifiers. Report server in `server/`
  (Cloudflare Worker, stores no IP). Nothing is sent until the server address is set in `report.ps1`.

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
