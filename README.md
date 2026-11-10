# XP Optimus Bypass

A free Vulkan layer for **X-Plane 12 on Windows Optimus laptops**: the NVIDIA GPU renders, the Intel iGPU drives the
screen. NVIDIA's own copy to the Intel screen is slow on these laptops; the layer shows each frame through the Intel
GPU itself. On the author's ASUS TUF F15 (GTX 1650) at KSEA: **about 22 → 42 FPS**, with far fewer stutters.

[繁體中文](README.zh-TW.md) · [Details](docs/DETAILS.md) · [Changelog](CHANGELOG.md) · [Releases](https://github.com/jjjay0826/xp-optimus-bypass/releases)

## Download

**[XP_Optimus_Bypass_v0.9.3.2.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.3.2/XP_Optimus_Bypass_v0.9.3.2.zip)**
([release notes](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.3.2)). Tested only on the author's
laptop so far; testers with other laptops are very welcome.

## Requirements

- Windows 10/11, X-Plane 12 (Vulkan) rendering on an NVIDIA GeForce GPU.
- X-Plane's screen is driven by the Intel iGPU: an Optimus laptop without a MUX switch, or with the MUX in hybrid mode.
- Any Intel generation with any GeForce should work in principle; so far only 10th gen UHD + GTX 1650 is tested.
- External monitors: if the port is wired to the Intel iGPU the bypass works there too; if it is wired to the NVIDIA
  GPU (common for HDMI on gaming laptops), the layer switches to NVIDIA's normal output on that screen by itself.
- Not for desktops, dGPU-only mode, AMD GPUs, Linux or macOS; there the layer stays inactive.

## Install and use

1. Extract the whole zip and run **`XP_Optimus_Bypass.bat`**.
2. Confirm the X-Plane path and the install folder, then click **Install**.
3. Start X-Plane with **Launch X-Plane** in the manager or the desktop shortcut (Steam copies: open Steam first). A
   normal X-Plane start does not load the layer in the recommended *X-Plane only* mode.
4. The first flight on a PC compares the two output paths for about a minute (brief stutters, half FPS twice); the
   faster one is remembered.

To remove it: **Uninstall** in the manager. If PowerShell is restricted, use the `fallback` folder in the zip.
Settings are in `app\xp_optimus_bypass.ini` in the install folder; see [Details](docs/DETAILS.md#settings).

## Performance

Author's ASUS TUF F15 (i5-10300H, GTX 1650, no MUX), Baron 58 at KSEA. Not a guarantee for other laptops.

| | Without the layer | With the layer |
|---|---|---|
| Median frame time (layer's own measurement) | 43.6–46.9 ms (~21–23 FPS) | 22.2–24.7 ms (~40–45 FPS) |
| Frames over 50 ms per 300 frames | 37–63 | 0–4 |

With the layer the GTX 1650 itself is the limit (Nsight Systems: busy ~86% of each frame). Comparable measurements:
[benchmark procedure](installer/docs/BENCHMARK.md); analysis: [measurements](docs/measurements.md).

## Anonymous reports

After X-Plane closes, the launcher sends one small report: GPU models, drivers, Windows / X-Plane version, FPS, which
output path was used, and a random ID made on your PC (resettable). No names, paths, IP addresses or hardware IDs. On by
default; switch it off or see the exact content in the manager. Public results:
<https://xpob-reports.jjjay0826.workers.dev/v1/summary> · [full description](docs/DETAILS.md#anonymous-compatibility-reports)

## Problems and limits

- Every wait in the layer has a time limit; if the Intel side fails it falls back to NVIDIA's normal output and
  X-Plane keeps running. A stuck driver gives X-Plane a lost-device error instead of a freeze.
- SDR only: the bypass does not carry HDR (X-Plane may be offered SDR formats only).
- Not tested yet: other laptops, external / multiple monitors, VR, Global mode. Known limits:
  [Details](docs/DETAILS.md#compatibility-notes-and-reporting-issues).
- Report problems on [GitHub Issues](https://github.com/jjjay0826/xp-optimus-bypass/issues) with your laptop model and
  `xp_optimus_bypass.log` (manager → **Logs / Readme** → **Report** copies a ready-made report).
- The DLL is not code-signed: [how to verify the download](docs/DETAILS.md#verify-the-download).

## License

[MIT](LICENSE). Developed by the author with help from Anthropic's Claude. Not affiliated with Laminar Research, NVIDIA
or Intel.
