# XP Optimus Bypass

A Vulkan layer for X-Plane 12 on Windows Optimus laptops where an NVIDIA GeForce GPU renders the game and an Intel integrated GPU drives the display. It presents completed frames through the Intel GPU to bypass NVIDIA's normal Optimus presentation path.

## Download

**Current release: [0.9.3-beta](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.3-beta)** — compatibility rework, tested on the author's ASUS TUF F15 (GTX 1650 + UHD Graphics, Windows 11). Other laptops have not been tested yet; the anonymous reports (see below) will show where it works.

Download **[XP_Optimus_Bypass_v0.9.3-beta.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.3-beta/XP_Optimus_Bypass_v0.9.3-beta.zip)** for the installer. The automatically generated “Source code” archives are for developers. The previous version, [0.9.2](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.2), stays available.

[繁體中文](README.zh-TW.md) · [Release history](https://github.com/jjjay0826/xp-optimus-bypass/releases) · [Installer changelog](installer/CHANGELOG.md) · [Quick guide / 英中操作說明](installer/README.txt) · [0.9.3 plan](docs/PLAN-0.9.3.md)

## What's new in 0.9.3-beta

- **Intel output through DXGI flip mode (D3D12).** The Intel GPU shows the frame through a flip-model swapchain and
  reads the transfer block in place, so switching to the native NVIDIA output and back no longer leaves a frozen
  picture (it did with older Intel Vulkan drivers). `intel_output=vulkan` keeps the 0.9.2-style output.
- **No freezes from a stuck Intel driver:** Intel waits are time-limited and fall back to the native path; a
  watchdog writes `[hang]` lines if no frame is shown.
- **Steam copies start directly** (a `steam_appid.txt` is added next to `X-Plane.exe`), so Steam no longer restarts
  X-Plane without the layer.
- **Path switching inside the layer.** X-Plane always renders into the layer's images. Each frame goes either
  through the Intel GPU (*bypass*) or into a real NVIDIA swapchain owned by the layer (*native*). Moving the window
  to a screen driven by the NVIDIA GPU, an Intel-side failure or the measurement below switch the path without
  X-Plane recreating its swapchain.
- **Measured path choice.** With `path=auto`, the first time on a setup the layer compares native and bypass
  (native / bypass / native / bypass, 300 frames each, in flight: 30 s after the loading screen, never in menus) and keeps the bypass only
  if its median frame time is at least 5% lower and its 95th percentile is not more than 10% worse. The result is
  cached per GPU, driver and screen in `xpob_path_cache.txt`. During this one-time measurement (about a minute) the
  frame rate drops to about half twice and the picture stutters briefly at each switch; the installer says so.
- **Compatibility:** the Intel GPU driving X-Plane's screen is chosen by LUID; when a GPU cannot import host
  memory, a CPU copy is used instead; when there is no spare graphics queue, a transfer queue is used; formats
  other than BGRA8 are converted and different sizes are scaled with blits; X-Plane is offered only SDR formats
  the bypass can handle.
- **Diagnostics:** `[diag]` log lines list every GPU, LUID, extension, queue family, screen, swapchain request,
  registered Vulkan layer and loaded overlay DLL. Every 300 frames the log records frame-time **P50 / P95 / P99**,
  maximum and frames over 50 ms, plus GPU copy, CPU copy, fence wait, acquire and present times for the active path.
- **Test switches** in the INI let one laptop exercise every code path (see Settings).
- **Installer:** Global mode is now an advanced option with an anti-cheat warning; the installer detects a ZIP that
  was not extracted and restricted PowerShell, writes more detail to `installer-error.log`, and the ZIP has a
  `fallback` folder that installs without PowerShell.
- **Anonymous compatibility reports** (on by default, can be switched off in the manager; see below).

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

1. Fully extract `XP_Optimus_Bypass_v0.9.3-beta.zip`. Keep the BAT file and the `app`, `docs` and `fallback` folders together.
2. Close X-Plane and run **`XP_Optimus_Bypass.bat`**.
3. Confirm the **X-Plane installation path**. Select the intended installation from the list, use **Browse**, or enter the path to `X-Plane.exe` or its containing folder.
4. Choose a separate folder in which to install **XP Optimus Bypass**. The default is `%LOCALAPPDATA%\Programs\XP Optimus Bypass`. Choose a location your Windows account can write to.
5. Select an activation mode, set the desktop-shortcut, launch-after-install and report options, then click **Install**. A notice explains the default-on anonymous reports and the one-time path measurement before anything is installed.

If the installer cannot start (for example, restricted PowerShell), use the `fallback` folder: `install.bat`, then `start_xplane.bat`; `uninstall.bat` removes it.

| Activation mode | How to start X-Plane |
|---|---|
| **X-Plane only — recommended** | Use **Launch X-Plane** in the manager or the dedicated shortcut. These enable the layer before starting the game. An ordinary X-Plane launch does not enable it. For Steam copies, the launcher adds a `steam_appid.txt` next to `X-Plane.exe` so Steam does not restart the game without the layer (Steam must be running; removed on uninstall). |
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
XP Optimus Bypass 0.9.3-beta (build ...) loaded in X-Plane.exe | ...
[diag] Intel output: DXGI flip model through D3D12 on Intel(R) ... Graphics | ...
ACTIVE: X-Plane ... -> GeForce -> host memory -> Intel -> screen
```

The `build` id identifies the exact DLL. `[hang]` lines mean no frame was shown for several seconds (they say whether
X-Plane itself was busy, for example loading, or where the layer was waiting); `[slow]` lines report a slow present.

FPS records alone do not prove the bypass is active: FPS is also recorded in `mode=off`. Windows build, X-Plane executable version and NVIDIA/Intel driver information are logged when available.

If there is no DLL output, first check that you are viewing the installed folder's log. In X-Plane-only mode, use the dedicated launcher. Then inspect `launcher-diagnostics.log`; do not assume the layer loaded just because X-Plane started.

## Settings

Edit **`app/xp_optimus_bypass.ini` inside the installed folder**, then restart X-Plane.

| Setting | Values |
|---|---|
| `mode` | `auto` — enable when the required GPU/display arrangement is detected; `on` — force an attempt; `off` — disable the bypass while retaining FPS logging when `stats=1`. |
| `vsync` | `0` — default, no refresh-rate cap and possible tearing; `1` — enable vertical synchronization. |
| `stats` | `1` — default, record FPS and timings every 300 frames; `0` — disable performance-statistics logging. |
| `path` (0.9.3) | `auto` — default: bypass while the window is on an Intel-driven screen (after the measured comparison), native otherwise; `bypass` / `native` — force one path; `cpu-copy` — bypass with a CPU copy (testing). |
| `probe` (0.9.3) | `1` — default: measure native vs bypass once per setup and keep the faster one (needs the DXGI Intel output); `0` — use the bypass whenever the window is on the Intel screen. Delete `xpob_path_cache.txt` to measure again. |
| `intel_output` (0.9.3) | `auto` — default: the Intel GPU shows the picture through DXGI flip mode (D3D12), else Vulkan; `dxgi`; `vulkan` — the 0.9.2-style Intel Vulkan output (after native output on the same window the bypass cannot come back). |
| `test_*` (0.9.3) | Test switches, leave at `0`: `test_force_blit`, `test_force_cpu_copy`, `test_no_extra_queue`, `test_fail_intel_after=N` (simulates an Intel failure after N bypass frames). |

The INI `mode` setting controls bypass behavior. It is separate from the installer's **X-Plane only / Global** choice, which controls how the DLL is loaded.

## Uninstall

Close X-Plane, open the manager and click **Uninstall**. Alternatively, remove **XP Optimus Bypass** through Windows Settings → Apps.

If the DLL is still in use, close the relevant Vulkan applications and retry. After uninstalling and closing the manager, you may also delete the extracted download folder and the separate setup cache at `%LOCALAPPDATA%\XP Optimus Bypass Installer` if you no longer need them.

## Performance results

These measurements were made on the author's ASUS TUF F15, with an i5-10300H, GTX 1650 and no MUX switch. **They are not a guarantee for other systems.**

| Test | Layer off / native | Layer on (bypass) |
|---|---|---|
| Baron 58 at KSEA, 0.9.3-beta measured comparison (`[probe]` log lines, median frame time) | 43.6–46.9 ms (about 21–23 FPS) | 22.2–24.7 ms (about 40–45 FPS) |
| Baron 58 at KSEA 16L, clear weather, 12:00 local (`0.9.1-beta`) | 21.3 FPS median | 34.9 FPS median |
| Frames over 50 ms per 300 frames, same KSEA test | 37–63 | 0–4 |
| Baron 58 at RCYU, local midnight (early test build) | About 20 FPS | 40–45 FPS |
| A330 at RCYU, local midnight (`0.9.0-beta`) | Under 19 FPS | 36–41 FPS |

The RCYU night scenes are lighter than the KSEA noon scenario and are not directly comparable. In the reported KSEA test, X-Plane's CPU work became the remaining limit after enabling the layer.

For comparable measurements, follow the [benchmark procedure](installer/docs/BENCHMARK.md) and retain both `mode=off` and `mode=auto` logs. Use the dedicated launcher for both runs when installed in X-Plane-only mode.

## How it works

1. X-Plane renders into images managed by the layer.
2. A worker thread copies completed frames from the NVIDIA GPU into pinned system memory using `VK_EXT_external_memory_host`.
3. The Intel GPU reads that memory in place through D3D12 and presents the frame on X-Plane's window through a DXGI flip-model swapchain (0.9.3; `intel_output=vulkan` uses an Intel Vulkan swapchain as in 0.9.2). Because NVIDIA's own output also uses flip mode, the layer can switch between the two at any time.

If Intel-side presentation fails, the layer switches to its native NVIDIA output for the rest of the session (0.9.3: inside the layer, without X-Plane recreating its swapchain). The performance benefit is then lost, but X-Plane keeps running.

See the [measurements](docs/measurements.md) and [investigation notes](docs/investigation_zh-TW.md) for the original analysis.

## Anonymous compatibility reports (0.9.3)

To find out on which laptops the layer works, the launcher can send one small anonymous report **after X-Plane
closes**. It is **on by default** and shown in the installer; switch it off at any time in the manager
(*Send anonymous compatibility reports*). *See what is sent* shows the exact content.

- **Sent:** layer and installer version, Windows build, X-Plane version, GPU models and driver versions, number of
  screens and whether X-Plane's screen is driven by the Intel GPU, the chosen path and why, failure reasons,
  median FPS and frame-time percentiles per path, names of Vulkan overlay layers, and whether test switches were used.
- **Not sent:** file paths, user name, computer name, the log itself, hardware serial numbers or LUIDs, or any
  identifier. Reports from the same PC are not linked to each other.
- The DLL never connects to the network; only the launcher does, once per session, and gives up on failure.
- The server ([`server/`](server/)) stores only those fields and the day; it does not store IP addresses or request headers.
- Aggregated results (per GPU combination) are public: <https://xpob-reports.jjjay0826.workers.dev/v1/summary>

The manager's log window also has **Report**, which shows the same report so you can copy it into a forum post or a
GitHub issue.

## Compatibility notes and reporting issues

- `vsync=0` can produce tearing. Try `vsync=1` if needed.
- Global mode loads an unsigned DLL into other Vulkan applications and may conflict with anti-cheat software. Use X-Plane-only mode for those environments and restart affected applications after changing modes.
- Vulkan overlays or capture tools may cause compatibility problems. Disable them when investigating a crash.
- These tests do not establish compatibility with VR, every multi-monitor arrangement, long add-on-heavy sessions or all GPU/driver combinations.
- Before reporting an X-Plane issue to Laminar Research, check whether it also occurs with this layer uninstalled.

Report issues through [GitHub Issues](https://github.com/jjjay0826/xp-optimus-bypass/issues). Include the release version, laptop model, CPU/GPU, Windows and driver versions, activation mode, relevant logs and steps to reproduce. For performance comparisons, include graphics settings and both benchmark logs.

## Verify the download

The DLL is not digitally signed. From the **extracted package's root folder**, run:

```bat
certutil -hashfile app\xp_optimus_bypass.dll SHA256
```

Compare the result with the `app/xp_optimus_bypass.dll` entry in the packaged `SHA256SUMS.txt` (the entry may start with `./`). Official GitHub Actions release builds also provide build-provenance attestations:

```sh
gh attestation verify XP_Optimus_Bypass_v0.9.3-beta.zip -R jjjay0826/xp-optimus-bypass
```

Attestation applies to the exact archive built by the workflow; it does not carry over to a modified or repackaged ZIP.

## Build from source

From a source checkout, using the MinGW-w64 toolchain described in `build.sh`:

```sh
sh build.sh
sh package.sh 0.9.3-beta
```

The output is `XP_Optimus_Bypass_v0.9.3-beta.zip`.

- Vulkan layer source: [`src/xp_optimus_bypass.cpp`](src/xp_optimus_bypass.cpp)
- Installer sources and documentation: [`installer/`](installer/)
- Build and release workflow: [`.github/workflows/build.yml`](.github/workflows/build.yml)

## Credits and license

The original investigation and layer implementation were developed by the author with assistance from Anthropic's Claude, using Intel VTune, NVIDIA Nsight Systems, PresentMon and experimental layers.

[MIT License](LICENSE). Not affiliated with Laminar Research, NVIDIA or Intel.
