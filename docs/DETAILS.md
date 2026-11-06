# XP Optimus Bypass — details

Everything that does not fit in the short [README](../README.md): installation options, logs, every setting, how it works, the anonymous reports in full, known limits, testing, verification and building. [繁體中文](DETAILS.zh-TW.md)

## Installation

1. Fully extract `XP_Optimus_Bypass_v0.9.3.2.zip`. Keep the BAT file and the `app`, `docs` and `fallback` folders together.
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
XP Optimus Bypass 0.9.3 (build ...) loaded in X-Plane.exe | ...
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
| `test_*` (0.9.3) | Test switches, leave at `0`: `test_force_blit`, `test_force_cpu_copy`, `test_no_extra_queue`, `test_fail_intel_after=N` (simulates an Intel failure after N bypass frames), `test_hang_worker_after=N` (the worker hangs forever at its Nth frame), `test_fail_acquire_submit_every=N` (every Nth acquire submit fails). |

The INI `mode` setting controls bypass behavior. It is separate from the installer's **X-Plane only / Global** choice, which controls how the DLL is loaded.

## Uninstall

Close X-Plane, open the manager and click **Uninstall**. Alternatively, remove **XP Optimus Bypass** through Windows Settings → Apps.

If the DLL is still in use, close the relevant Vulkan applications and retry. After uninstalling and closing the manager, you may also delete the extracted download folder and the separate setup cache at `%LOCALAPPDATA%\XP Optimus Bypass Installer` if you no longer need them.

## How it works

1. X-Plane renders into images managed by the layer.
2. A worker thread copies completed frames from the NVIDIA GPU into pinned system memory using `VK_EXT_external_memory_host`.
3. The Intel GPU reads that memory in place through D3D12 and presents the frame on X-Plane's window through a DXGI flip-model swapchain (0.9.3; `intel_output=vulkan` uses an Intel Vulkan swapchain as in 0.9.2). Because NVIDIA's own output also uses flip mode, the layer can switch between the two at any time.

If Intel-side presentation fails, the layer switches to its native NVIDIA output for the rest of the session (0.9.3: inside the layer, without X-Plane recreating its swapchain). The performance benefit is then lost, but X-Plane keeps running.

See the [measurements](measurements.md) and [investigation notes](investigation_zh-TW.md) for the original analysis.

## Anonymous compatibility reports

To find out on which laptops the layer works, the launcher can send one small anonymous report **after X-Plane
closes**. It is **on by default** and shown in the installer; switch it off at any time in the manager
(*Send anonymous compatibility reports*). *See what is sent* shows the exact content.

- **Sent:** a random anonymous installation ID (see below), layer and installer version, Windows build, X-Plane version, GPU models and driver versions, number of
  screens and whether X-Plane's screen is driven by the Intel GPU, the chosen path and why, failure reasons,
  median FPS and frame-time percentiles per path, names of Vulkan overlay layers, and whether test switches were used.
- **Not sent:** file paths, user name, computer name, the log itself, hardware serial numbers or LUIDs.
- **Anonymous ID (installer 0.9.3.2):** a random number made on your PC the first time a report is built
  (`app\report_id.txt`). It is not derived from your hardware or from you; it only lets reports from the same
  installation be grouped (for example before and after a driver update). *Reset anonymous ID* in the report window
  replaces it; uninstalling removes it. It never appears in the public summary, which only counts installations.
- The DLL never connects to the network; only the launcher does, once per session, and gives up on failure.
- The server ([`server/`](../server/)) stores only those fields and the day; it does not store IP addresses or request headers.
- Aggregated results (per GPU combination) are public: <https://xpob-reports.jjjay0826.workers.dev/v1/summary>

The manager's log window also has **Report**, which shows the same report so you can copy it into a forum post or a
GitHub issue.

## Compatibility notes and reporting issues

- `vsync=0` can produce tearing. Try `vsync=1` if needed.
- Global mode loads an unsigned DLL into other Vulkan applications and may conflict with anti-cheat software. Use X-Plane-only mode for those environments and restart affected applications after changing modes.
- Vulkan overlays or capture tools may cause compatibility problems. Disable them when investigating a crash.
- These tests do not establish compatibility with VR, every multi-monitor arrangement, long add-on-heavy sessions or all GPU/driver combinations.
- Known limits of the layer's virtual swapchain: a present with several swapchains (X-Plane does not do this) makes
  X-Plane fall back to real NVIDIA swapchains for the session, and present extension structs are dropped in that
  case; Vulkan device groups (`deviceMask`) are not supported; `VK_GOOGLE_display_timing` reports the real refresh
  rate but no past presentation timings; the NVIDIA low-latency (Reflex) calls reach the driver only for real swapchains; for the layer's they return at once.
- If a driver call inside the layer's worker thread never returns, the layer gives up after 30 s: X-Plane gets a lost
  device (error) instead of freezing, and the layer's objects are left allocated until X-Plane exits.
- Before reporting an X-Plane issue to Laminar Research, check whether it also occurs with this layer uninstalled.

Report issues through [GitHub Issues](https://github.com/jjjay0826/xp-optimus-bypass/issues). Include the release version, laptop model, CPU/GPU, Windows and driver versions, activation mode, relevant logs and steps to reproduce. For performance comparisons, include graphics settings and both benchmark logs.

## How releases are tested

GitHub Actions builds and packages every release reproducibly, checks the installer scripts on Windows (syntax,
English/Chinese text, install-path safety, report privacy) and attests the build. **It does not run a GPU, Vulkan,
D3D12 or X-Plane.** Those tests are done by hand on the author's laptop before a release; the results are in
[installer/docs/TESTING.md](../installer/docs/TESTING.md), including what has not been tested.

## Verify the download

The DLL is not digitally signed. Compare the ZIP's SHA-256 (`Get-FileHash XP_Optimus_Bypass_v*.zip` in PowerShell)
with the one in the release notes; the first line of `xp_optimus_bypass.log` shows the DLL's version and build id.
To check the DLL itself, run from the **extracted package's root folder**:

```bat
certutil -hashfile app\xp_optimus_bypass.dll SHA256
```

Compare the result with the `app/xp_optimus_bypass.dll` entry in the packaged `SHA256SUMS.txt` (the entry may start with `./`). Official GitHub Actions release builds also provide build-provenance attestations:

```sh
gh attestation verify XP_Optimus_Bypass_v0.9.3.2.zip -R jjjay0826/xp-optimus-bypass
```

Attestation applies to the exact archive built by the workflow; it does not carry over to a modified or repackaged ZIP.

## Build from source

From a source checkout, using the MinGW-w64 toolchain described in `build.sh`:

```sh
sh build.sh
sh package.sh 0.9.3.2
```

The output is `XP_Optimus_Bypass_v0.9.3.2.zip`.

- Vulkan layer source: [`src/xp_optimus_bypass.cpp`](../src/xp_optimus_bypass.cpp)
- Installer sources and documentation: [`installer/`](../installer/)
- Build and release workflow: [`.github/workflows/build.yml`](../.github/workflows/build.yml)
