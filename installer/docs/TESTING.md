# 0.9.3 acceptance / 0.9.3 實機驗收

Each step: start X-Plane with the dedicated launcher, fly about 2 minutes at KSEA 16L (benchmark scenario), close
X-Plane, keep the log. Change only the listed INI lines, then set them back. / 每一步都用專用入口啟動，
在 KSEA 16L 飛約 2 分鐘後關閉並保存 log；只改列出的 INI 設定，測完改回。

| # | INI | Expected in the log / log 應該出現 |
|---|---|---|
| 1 | defaults | `[diag]` lines (GPUs with LUID, screens, queue families, swapchain request); `path -> bypass (start)`; `ACTIVE`; FPS lines with `path=bypass mem=shared` and P50/P95/P99; FPS about the same as 0.9.2 |
| 2 | `probe=1` + `test_bypass_after_native=2`, fly ~6 min (picture must not freeze after switching back; if it does, try `=1`) | `probe:` lines, 4 measurements, then `selected bypass` (or native) with numbers; restart X-Plane: `path from earlier measurement` |
| 3 | `path=native` | `path -> native`; FPS like without the layer (~20 at KSEA); `n_acq`/`n_pres` timings |
| 4 | `test_force_blit=1` | `GeForce conversion blit to BGRA8`; picture and colors correct; FPS only slightly lower |
| 5 | `test_force_cpu_copy=1` | `memory CPU copy (no host import)`; `cpu_copy` timings; picture correct |
| 6 | `test_no_extra_queue=1` | `transfer-only family`; bypass still works (no blit) |
| 7 | `test_fail_intel_after=600` | `simulating an Intel device loss` → `switching to the native NVIDIA path`; X-Plane keeps running, no crash |
| 8 | defaults, resize the X-Plane window / toggle fullscreen | picture follows the size (scaled while sizes differ), no crash |
| 9 | defaults, minimize and restore | no crash, picture returns |
| 10 | defaults + Steam overlay (Shift+Tab), RTSS, OBS (if available) | overlay names in `[diag]`; note whether the overlay shows and whether OBS records |
| 11 | manager: report checkbox on, fly, close X-Plane | `launcher-diagnostics.log`: report sent / not sent (no server yet); Report window shows no paths or user name |
| 12 | installer | Global hidden until Advanced is ticked; confirmation dialog; fallback folder install / start / uninstall works |
| 13 | Steam copy: launcher (Steam running / not running) | `launcher-diagnostics.log`: `Steam direct start: created ...steam_appid.txt (2014780)`, DLL log present; without Steam: "start Steam first"; uninstall removes that `steam_appid.txt` |

# Results / 驗收結果 (2026-11-04)

ASUS TUF F15: i5-10300H, GTX 1650 (driver 595.97), UHD Graphics (driver 101.1999), Windows 11 build 26300,
X-Plane 12.4.3. / 作者筆電實測。

| Area | Result |
|---|---|
| Bypass, DXGI output, in-place import | pass: ~40-45 FPS vs native ~21-23 FPS at KSEA |
| Native path; probe; switching native <-> bypass | pass (DXGI output); with the Intel Vulkan output the picture froze after native -> fixed by DXGI output |
| `test_fail_intel_after=600` | pass: switched to native, X-Plane kept running |
| `test_force_cpu_copy=1` + `test_no_extra_queue=1` | pass: CPU copy ~1.2 ms on the worker, transfer-only queue, ~40 FPS |
| Window resize (incl. 1843 px wide, padded rows), minimize, Alt+Tab, windowed/fullscreen | pass |
| Probe in flight only (menu / loading screen ignored) | pass after two fixes (see CHANGELOG) |
| Installer notice, fresh install to default folder, uninstall, fallback folder install/start/uninstall | pass |
| No leftover console windows; Steam direct start; anonymous report sent | pass |
| 0.9.3.1: timed DXGI back-buffer wait, bounded NVIDIA waits | pass: ~41-44 FPS, no timeouts or `[hang]` outside loading |
| Not tested: `test_force_blit`, Global mode, external / multiple monitors, HDR, VR, other laptops | - |

# Runtime acceptance / 實機驗收

The table above lists what was run on Windows with X-Plane. / 上表為已在 Windows 實測的項目。

1. English defaults; switch to Traditional Chinese and back while retaining settings.
2. Install, uninstall and reinstall in one window, then reopen the original BAT.
3. Test X-Plane-only and global modes; verify exactly one manifest is enabled.
4. Open Live logs before launch: the viewer must not create fake DLL records.
5. Start X-Plane with the dedicated button. Check DLL startup records appear before exit.
6. Observe FPS output after 300 rendered frames; confirm the viewer updates without reopening.
7. Check the real path is the installed root, not the extracted download folder.
8. Disable/remove layer registration: launcher must report the problem, not claim activation.
9. Trigger missing DLL output: inspect launcher-diagnostics.log and the actual loaded module.
10. Repeat in global mode with a normal game launch, without the dedicated launcher environment.
11. Upgrade from the hard-link installer; new root log and INI must work without hard-link creation.
12. Test concurrent reading while DLL appends and when X-Plane starts a fresh log.
13. Preserve paired benchmark logs before restarting. See [benchmark](BENCHMARK.md).
14. Verify uninstall preserves the independent installer source and never removes X-Plane.

1. 預設英文，來回切換繁體中文時保留輸入與勾選。
2. 同一視窗安裝、移除、重裝，並可重開原 BAT。
3. 僅 X-Plane 與全域模式只能啟用一個 manifest。
4. 啟動遊戲前開啟即時檢視，不得預先寫入假冒 DLL 的紀錄。
5. 遊戲未退出前就應出現 DLL 啟動紀錄。
6. 畫面經過 300 幀應有 FPS 統計，檢視視窗不用重開。
7. 紀錄位於正式安裝位置第一層，不是下載解壓縮資料夾。
8. 移除或停用 layer 登記時應明確提示，不得宣稱加速已啟用。
9. 沒有 DLL 紀錄時檢查獨立啟動診斷與實際載入模組。
10. 全域模式從一般方式開遊戲，也能直接寫第一層 log。
11. 舊硬連結版本升級後 INI 保留，log 不再依賴建立硬連結。
12. 測試 DLL 邊寫邊讀，以及遊戲重建 log 時的共享讀取。
13. 重開前保留效能比較需要的兩份 log。
14. 移除後保留獨立安裝來源，不動 X-Plane。

## Custom destination regression / 自訂位置回歸

- Install to a new custom folder, including spaces, Chinese and a different drive.
- Repeat using an empty folder created by the picker.
- Restart the manager and verify the registered custom location is restored.
- Verify layer manifest, shortcut working directory, Windows Apps uninstall and root logs use the custom path.
- Reject changing the active install location; uninstall, choose a different folder and reinstall successfully.
- Reject drive/profile/system roots, installer-source ancestors/descendants and unrelated nonempty folders.
- 新位置、空資料夾、中文／空白與不同磁碟皆須驗收；重開視窗可恢復位置。
- 登記、捷徑、移除、log 必須使用自訂位置；換位置先移除再重裝。
