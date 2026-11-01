# XP Optimus Bypass (beta)

A small Vulkan layer that can raise X-Plane 12's frame rate a lot on **Optimus laptops without a MUX switch**, by sending finished frames to the screen through the Intel iGPU yourself instead of through NVIDIA's built-in Optimus path.

On the author's laptop (ASUS TUF F15, i5-10300H + GTX 1650, no MUX):

| Aircraft | Without the layer | With the layer |
|---|---|---|
| Baron 58 (default) | ~20 FPS | 40–45 FPS |
| A330 (default) | under 19 FPS | 36–41 FPS |

Baron 58 "with" was measured with a pre-release test build, A330 with 0.9.0-beta; similar but not identical conditions. Results following the fixed [benchmark procedure](BENCHMARK.md) will replace these.

> This is a beta tested on **one** laptop. Please report results (good or bad) with your log file.

## Who it is for

It can help if **all** of these are true:

- Windows laptop, X-Plane 12 (Vulkan)
- NVIDIA GeForce renders X-Plane
- the built-in screen is driven by the **Intel** iGPU (Optimus, no MUX switch, or MUX set to hybrid)
- especially Intel **10th gen and older**, which cannot use Windows 11's Cross-Adapter Scan-Out (CASO)

It does nothing useful (and turns itself off in `mode=auto`) on desktops, on laptops with the MUX set to dGPU, or when X-Plane's window is on an external monitor plugged into the GeForce. Laptops with Intel 11th gen or newer have CASO; the gain there is unknown.

Not supported: AMD iGPUs, AMD/Intel discrete GPUs, Linux, macOS.

## Why it helps

On these laptops every frame has to travel GeForce → shared memory → Intel iGPU → screen. On the test laptop, NVIDIA's own path for this took about **47 ms per frame** while flying (about 0.5 ms in the main menu), so X-Plane could never get past ~21 FPS, no matter the settings. Moving the presentation calls to another thread didn't help: the path itself had a throughput of ~21 frames per second.

This layer replaces that path:

1. X-Plane renders into images owned by the layer (it never gets a real NVIDIA swapchain).
2. A worker thread copies each finished frame from the GeForce into pinned system memory (`VK_EXT_external_memory_host`).
3. A separate Vulkan device on the Intel iGPU copies it into its own swapchain on X-Plane's window and presents it.

The GeForce copy takes ~1–2 ms; the Intel present takes ~0.02 ms. After this, the GeForce itself becomes the limit, so graphics settings start to matter again.

All measurements (PresentMon, VTune, Nsight, and per-call timings of every test variant) are in [`docs/measurements.md`](docs/measurements.md).

## Install

1. Download the release and unzip it to a folder you will keep, e.g. `C:\XP_Optimus_Bypass`. **Don't move the folder afterwards** (the install points Windows to it).
2. Double-click `install.bat`.
3. Start X-Plane normally and open `xp_optimus_bypass.log` in the same folder. You should see:
   ```
   bypass prepared (extra GeForce queue: ...)
   Intel: device ready ...
   ACTIVE: X-Plane 1920x1080 -> GeForce -> host memory -> Intel -> screen
   ```
   and every ~10 seconds a line with `FPS ...`.

**Checking the download (optional):** the DLL is not code-signed, so some antivirus tools or Windows SmartScreen may warn about it. You can check it matches the published build: open a command prompt in the folder and run
```
certutil -hashfile xp_optimus_bypass.dll SHA256
```
The result must equal the value in `SHA256SUMS.txt` (and in the release notes). You can also build it yourself from `src/` (see below).

## Uninstall

Double-click `uninstall.bat` **in the same folder**, then delete the folder.

## Settings (`xp_optimus_bypass.ini`)

| Setting | Values |
|---|---|
| `mode` | `auto` (default): only when needed · `on`: always try · `off`: bypass disabled, only measures FPS |
| `vsync` | `0` (default): no cap, may tear · `1`: no tearing, FPS capped at the screen's refresh rate |
| `stats` | `1` (default): write FPS and timings to the log every 300 frames |

Restart X-Plane after changing anything.

## Known issues

- **Tearing** with `vsync=0` (the Intel side presents immediately). Use `vsync=1` if it bothers you.
- Not yet tested: VR, multiple monitors, moving the window between screens, long sessions with many add-ons, other GPUs/drivers.
- The DLL is **not code-signed**; antivirus or SmartScreen may warn. Check the SHA-256 as described above.
- Overlays that hook Vulkan (Steam overlay, Overwolf, OBS game capture, RTSS) may not play well with it. If you see crashes, disable them first.
- A future X-Plane or driver update may break it. Uninstall if X-Plane misbehaves.
- **Before reporting a bug to Laminar Research, uninstall this layer and reproduce without it.** Laminar did not make this and cannot support it.

## Reporting results

Please follow [BENCHMARK.md](BENCHMARK.md) (about 10 minutes: same scenario with `mode=off` and `mode=auto`) and open a **Benchmark result** issue with both logs. For problems, open a **Bug report** issue. Changes between versions are in [CHANGELOG.md](CHANGELOG.md).

## Build from source

```
./build.sh        # MinGW-w64 cross-compile (Linux/WSL)
```

Single source file: `src/xp_optimus_bypass.cpp`, plus the Khronos Vulkan headers.

## Credits

Investigation and code were done by the author together with an AI assistant (Anthropic's Claude), using Intel VTune, NVIDIA Nsight Systems, PresentMon and step-by-step test layers. The full investigation notes (Traditional Chinese) are in `docs/`.

MIT License. Not affiliated with Laminar Research, NVIDIA or Intel.
