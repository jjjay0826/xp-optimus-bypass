# Changelog

## 0.9.1-beta — not released yet (code on `main`)

- **Clean shutdown:** when X-Plane destroys its Vulkan device, the worker thread is stopped and joined and the Intel
  Vulkan device, surface and instance are released. Previously they were left running until the process ended, which
  could crash X-Plane on exit if the Vulkan loader unloaded the layer first.
- **FPS logging when the bypass is not active** (`mode=off`, or `mode=auto` on a system where it does not apply), so
  off/on comparisons use the same measurement. Log lines start with `FPS ... | bypass NOT active`.
- New [BENCHMARK.md](BENCHMARK.md): a fixed test procedure for comparable results.
- README: note that the DLL is not code-signed, and how to check the SHA-256 checksum.
- Release zip now contains `SHA256SUMS.txt`.
- GitHub issue templates for benchmark results and bug reports.

## 0.9.0-beta — 2026-11-01

- First public beta: presents X-Plane 12 frames through the Intel iGPU instead of NVIDIA's Optimus present path.
- `mode=auto|on|off`, `vsync`, `stats` settings; automatic detection of NVIDIA render GPU + Intel-driven display.
- Handles window resize/minimize by recreating the Intel swapchain internally.
- Per-300-frame log with FPS, average and worst-frame timings for each stage.
