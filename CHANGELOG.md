# Changelog

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
