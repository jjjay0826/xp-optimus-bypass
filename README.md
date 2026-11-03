# XP Optimus Bypass

A Vulkan layer for X-Plane 12 on Windows Optimus laptops where an NVIDIA GeForce GPU renders the game and an Intel integrated GPU drives the display. It presents completed frames through the Intel GPU to bypass NVIDIA's normal Optimus presentation path.

## Download

**Current release: [0.9.2](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.2)**

Download **[XP_Optimus_Bypass_v0.9.2.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.2/XP_Optimus_Bypass_v0.9.2.zip)** for the installer. The automatically generated “Source code” archives are for developers.

[Release history](https://github.com/jjjay0826/xp-optimus-bypass/releases) · [Installer changelog](installer/CHANGELOG.md) · [Quick guide / 英中操作說明](installer/README.txt)

## What's new in 0.9.2

- A graphical installer and manager, with English selected by default and Traditional Chinese available.
- Automatic detection of **X-Plane installation paths**, including Steam libraries, with manual selection available.
- A selectable installation folder for **XP Optimus Bypass**.
- A live viewer for the DLL log and launcher diagnostics.
- A single entry point, `XP_Optimus_Bypass.bat`, for installation and management.

This release also includes the X-Plane-only activation mode, Intel-side failure fallback and additional system information introduced in 0.9.1.1-beta.

## Requirements

- Windows 10/11 with Windows PowerShell 5.1 and a desktop session.
- X-Plane 12 running with Vulkan on an NVIDIA GeForce GPU.
- The display used by X-Plane is driven by an Intel integrated GPU, typically on an Optimus laptop without a MUX switch or with its MUX set to hybrid mode.

In `mode=auto`, the bypass is disabled where the required GPU/display arrangement is not detected. A desktop, a laptop in dGPU-only mode, or an external monitor connected directly to the NVIDIA GPU is not the intended use case.

The original investigation used an Intel 10th-generation laptop. Benefits on other hardware, including newer hybrid-display configurations, depend on the system. AMD integrated GPUs, AMD/Intel discrete render GPUs, Linux and macOS are not supported by this implementation.

## Installation

1. Fully extract `XP_Optimus_Bypass_v0.9.2.zip`. Keep the BAT file, `app` folder and `docs` folder together.
2. Close X-Plane and run **`XP_Optimus_Bypass.bat`**.
3. Confirm the **X-Plane installation path**. Select the intended installation from the list, use **Browse**, or enter the path to `X-Plane.exe` or its containing folder.
4. Choose a separate folder in which to install **XP Optimus Bypass**. The default is `%LOCALAPPDATA%\Programs\XP Optimus Bypass`. Choose a location your Windows account can write to.
5. Select an activation mode, set the desktop-shortcut and launch-after-install options, then click **Install**.

| Activation mode | How to start X-Plane |
|---|---|
| **X-Plane only — recommended** | Use **Launch X-Plane** in the manager or the dedicated shortcut. These enable the layer before starting the game. An ordinary X-Plane launch does not enable it. |
| **Global** | Start X-Plane normally. Vulkan applications under the current Windows account load the DLL, but the bypass operates only inside `X-Plane.exe`. |

The desktop shortcut is optional. In X-Plane-only mode, it provides a direct way to launch the game with the layer enabled, without first opening the manager. Global mode does not require that shortcut.

Automatic discovery runs in the background. If known locations produce no result, it searches local fixed drives; this can take several minutes. Network and removable-drive installations can be selected manually.

After installation succeeds, the extracted download folder may be deleted. Keep the selected installation folder in place. To change an existing installation's location, uninstall the tool first, then reinstall it in the new folder. Back up any settings and logs you want to retain before uninstalling.

## Logs and diagnostics

Logs are written to the **top level of the folder selected for XP Optimus Bypass**, not the extracted download folder:

| File | Contents |
|---|---|
| `xp_optimus_bypass.log` | DLL initialization, bypass status, system information and performance statistics. |
| `launcher-diagnostics.log` | Launch requests, layer-registration checks and diagnostics when fresh DLL output is not detected. |

Click **Logs / Readme** in the manager to open the log viewer. The viewer rereads the files every second. With `stats=1`, FPS and timing statistics are recorded **every 300 frames**, so their interval depends on the frame rate.

Each DLL message is flushed when written. The DLL starts a fresh log for each X-Plane session; copy logs before restarting if you need to keep them.

When the bypass becomes active, look for records such as:

```text
bypass prepared (extra GeForce queue: ...)
Intel: device ready ...
ACTIVE: X-Plane ... -> GeForce -> host memory -> Intel -> screen
```

FPS records alone do not prove the bypass is active: FPS is also recorded in `mode=off`. Windows build, X-Plane executable version and NVIDIA/Intel driver information are logged when available.

If there is no DLL output, first check that you are viewing the installed folder's log. In X-Plane-only mode, use the dedicated launcher. Then inspect `launcher-diagnostics.log`; do not assume the layer loaded just because X-Plane started.

## Settings

Edit **`app/xp_optimus_bypass.ini` inside the installed folder**, then restart X-Plane.

| Setting | Values |
|---|---|
| `mode` | `auto` — enable when the required GPU/display arrangement is detected; `on` — force an attempt; `off` — disable the bypass while retaining FPS logging when `stats=1`. |
| `vsync` | `0` — default, no refresh-rate cap and possible tearing; `1` — enable vertical synchronization. |
| `stats` | `1` — default, record FPS and timings every 300 frames; `0` — disable performance-statistics logging. |

The INI `mode` setting controls bypass behavior. It is separate from the installer's **X-Plane only / Global** choice, which controls how the DLL is loaded.

## Uninstall

Close X-Plane, open the manager and click **Uninstall**. Alternatively, remove **XP Optimus Bypass** through Windows Settings → Apps.

If the DLL is still in use, close the relevant Vulkan applications and retry. After uninstalling and closing the manager, you may also delete the extracted download folder and the separate setup cache at `%LOCALAPPDATA%\XP Optimus Bypass Installer` if you no longer need them.

## Historical performance results

These measurements were reported on the author's ASUS TUF F15, with an i5-10300H, GTX 1650 and no MUX switch. **They are historical results from earlier builds, not new 0.9.2 benchmark results or a guarantee for other systems.**

| Test | Layer off | Layer on |
|---|---|---|
| Baron 58 at KSEA 16L, clear weather, 12:00 local (`0.9.1-beta`) | 21.3 FPS median | 34.9 FPS median |
| Frames over 50 ms per 300 frames, same KSEA test | 37–63 | 0–4 |
| Baron 58 at RCYU, local midnight (early test build) | About 20 FPS | 40–45 FPS |
| A330 at RCYU, local midnight (`0.9.0-beta`) | Under 19 FPS | 36–41 FPS |

The RCYU night scenes are lighter than the KSEA noon scenario and are not directly comparable. In the reported KSEA test, X-Plane's CPU work became the remaining limit after enabling the layer.

For comparable measurements, follow the [benchmark procedure](installer/docs/BENCHMARK.md) and retain both `mode=off` and `mode=auto` logs. Use the dedicated launcher for both runs when installed in X-Plane-only mode.

## How it works

1. X-Plane renders into images managed by the layer.
2. A worker thread copies completed frames from the NVIDIA GPU into pinned system memory using `VK_EXT_external_memory_host`.
3. A separate Vulkan device on the Intel GPU copies those frames into its swapchain and presents them on X-Plane's window.

If Intel-side presentation fails, the layer attempts to return to NVIDIA's normal presentation path for the rest of the session. The performance benefit may then be lost.

See the [measurements](docs/measurements.md) and [investigation notes](docs/investigation_zh-TW.md) for the original analysis.

## Compatibility notes and reporting issues

- `vsync=0` can produce tearing. Try `vsync=1` if needed.
- Global mode loads an unsigned DLL into other Vulkan applications and may conflict with anti-cheat software. Use X-Plane-only mode for those environments and restart affected applications after changing modes.
- Vulkan overlays or capture tools may cause compatibility problems. Disable them when investigating a crash.
- The historical benchmarks do not establish compatibility with VR, every multi-monitor arrangement, long add-on-heavy sessions or all GPU/driver combinations.
- Before reporting an X-Plane issue to Laminar Research, check whether it also occurs with this layer uninstalled.

Report issues through [GitHub Issues](https://github.com/jjjay0826/xp-optimus-bypass/issues). Include the release version, laptop model, CPU/GPU, Windows and driver versions, activation mode, relevant logs and steps to reproduce. For performance comparisons, include graphics settings and both benchmark logs.

## Verify the download

The DLL is not digitally signed. From the **extracted package's root folder**, run:

```bat
certutil -hashfile app\xp_optimus_bypass.dll SHA256
```

Compare the result with the `app/xp_optimus_bypass.dll` entry in the packaged `SHA256SUMS.txt` (the entry may start with `./`). Official GitHub Actions release builds also provide build-provenance attestations:

```sh
gh attestation verify XP_Optimus_Bypass_v0.9.2.zip -R jjjay0826/xp-optimus-bypass
```

Attestation applies to the exact archive built by the workflow; it does not carry over to a modified or repackaged ZIP.

## Build from source

From a source checkout, using the MinGW-w64 toolchain described in `build.sh`:

```sh
sh build.sh
sh package.sh 0.9.2
```

The output is `XP_Optimus_Bypass_v0.9.2.zip`.

- Vulkan layer source: [`src/xp_optimus_bypass.cpp`](src/xp_optimus_bypass.cpp)
- Installer sources and documentation: [`installer/`](installer/)
- Build and release workflow: [`.github/workflows/build.yml`](.github/workflows/build.yml)

## Credits and license

The original investigation and layer implementation were developed by the author with assistance from Anthropic's Claude, using Intel VTune, NVIDIA Nsight Systems, PresentMon and experimental layers.

[MIT License](LICENSE). Not affiliated with Laminar Research, NVIDIA or Intel.
