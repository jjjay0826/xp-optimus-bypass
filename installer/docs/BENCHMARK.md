# Benchmark procedure

Use this so results from different laptops can be compared. It takes about 10 minutes.
（中文說明在下方）

## What you need

- The DLL records FPS in both `mode=off` and `mode=auto` (since 0.9.1.1); the first log line shows its version
  and build id.
  For an X-Plane-only installation, use the installed launch shortcut or open
  `XP_Optimus_Bypass.bat` and press **啟動 X-Plane (Launch X-Plane)** for both runs.
  Settings are in `<install-root>/app/xp_optimus_bypass.ini`; the log is
  `<install-root>/xp_optimus_bypass.log`. See [README](../README.txt).
- Close other programs that use the GPU (browsers with video, OBS, Overwolf, etc.).
- Laptop plugged in, Windows power mode on "Best performance", same fan/performance profile for both runs.

## Fixed scenario

| Setting | Value |
|---|---|
| Aircraft | Default **Beechcraft Baron 58** (optional second run: default **Airbus A330-300**) |
| Airport | **KSEA** Seattle-Tacoma, runway **16L**, "ready for takeoff", engines running, parking brake set |
| Weather | Manual: **clear**, no clouds, no precipitation |
| Time | Same date, **12:00 local** |
| View | Default cockpit view. **Don't move the camera or the mouse** during the measurement |
| X-Plane graphics settings | Whatever you normally use — but **identical** in both runs. Take a screenshot of them for your report |

## Steps

1. In `app/xp_optimus_bypass.ini` set `mode=off`. Start X-Plane, load the scenario above.
2. Wait **60 seconds** after the scenario has loaded (scenery and shaders settle), then wait **another 60 seconds** without touching anything.
3. Quit X-Plane. Copy `xp_optimus_bypass.log` somewhere (it is rewritten on every start), e.g. `log_off.txt`.
4. Set `mode=auto`, repeat steps 1–3, save as `log_on.txt`.
5. For each log, take the `FPS` lines from the **second minute** (ignore the first minute) and report their **median**.
   Also note the largest `frames over 50 ms` count in that minute.

## Report

Open a "Benchmark result" issue on GitHub and include: laptop model, CPU, GPU, NVIDIA and Intel driver versions,
Windows version, screen resolution and refresh rate, the two median FPS values, both log files and the settings
screenshot. Results where it *doesn't* help are just as useful.

---

## 中文說明

讓不同筆電的結果可以互相比較，全程大約 10 分鐘。

**準備**：DLL 在 `mode=off` 與 `mode=auto` 都會記錄 FPS（0.9.1.1 起）；log 第一行會顯示版本和 build id。
選「僅 X-Plane」安裝時，兩次都要從專用捷徑，或 `XP_Optimus_Bypass.bat` 視窗內的
「啟動 X-Plane」按鈕啟動。設定在 `<安裝位置>/app/xp_optimus_bypass.ini`，log 在
`<安裝位置>/xp_optimus_bypass.log`，詳見 [README](../README.txt)。關掉其他會使用顯卡的程式；
接上電源，Windows 電源模式設為「最佳效能」，兩次使用相同散熱／效能模式。

**固定情境**：內建 **Baron 58**（可再加測內建 **A330-300**）；**KSEA** 西雅圖機場 **16L** 跑道，準備起飛、引擎開著、停機煞車拉上；天氣手動設成**晴天無雲**；時間**當地 12:00**；預設駕駛艙視角，量測時**不要動視角和滑鼠**；畫質設定照平常，但**兩次要完全一樣**，並截圖附在回報裡。

**步驟**：
1. `app/xp_optimus_bypass.ini` 設 `mode=off`，開 X-Plane 載入上面的情境
2. 載入完成後等 **60 秒**，再什麼都不碰等 **60 秒**
3. 關掉 X-Plane，把 `xp_optimus_bypass.log` 另存一份（每次啟動都會重寫），例如 `log_off.txt`
4. 改成 `mode=auto`，重複 1～3，另存成 `log_on.txt`
5. 每份 log 只取**第二分鐘**的 `FPS` 數值，回報**中位數**，並記下那一分鐘裡 `frames over 50 ms` 的最大值

**回報**：在 GitHub 開一個「Benchmark result」issue，附上筆電型號、CPU、顯卡、NVIDIA 與 Intel 驅動版本、Windows 版本、螢幕解析度與更新率、兩個 FPS 中位數、兩份 log 和畫質設定截圖。沒有效果的結果一樣很有用。
