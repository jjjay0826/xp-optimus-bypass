# Benchmark procedure

Use this so results from different laptops can be compared. It takes about 10 minutes.
（中文說明在下方）

## What you need

- XP Optimus Bypass installed. With **0.9.0-beta** the log has no FPS lines in `mode=off`: turn on X-Plane's own
  frame-rate display (Settings → Data Output → "Frame rate" → show in cockpit) and note the FPS every ~10 s instead.
  From 0.9.1-beta the log measures FPS in both modes. With the "X-Plane only" install (0.9.1.1+), start X-Plane with
  `start_xplane.bat` for **both** runs, also for `mode=off`, or there will be no log.
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

1. In `xp_optimus_bypass.ini` set `mode=off`. Start X-Plane, load the scenario above.
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

**準備**：安裝 XP Optimus Bypass。**0.9.0-beta** 在 `mode=off` 時 log 不會記 FPS，請打開 X-Plane 內建的 FPS 顯示（Settings → Data Output → 「Frame rate」→ 顯示在駕駛艙），每 10 秒左右記一次；0.9.1-beta 起兩種模式都會在 log 記錄 FPS。0.9.1.1 起如果安裝時選「只給 X-Plane」，**兩次**（包括 `mode=off`）都要用 `start_xplane.bat` 開 X-Plane，否則不會有 log。關掉其他會用到顯卡的程式；接上電源，Windows 電源模式設成「最佳效能」，兩次測試用同一個散熱／效能模式。

**固定情境**：內建 **Baron 58**（可再加測內建 **A330-300**）；**KSEA** 西雅圖機場 **16L** 跑道，準備起飛、引擎開著、停機煞車拉上；天氣手動設成**晴天無雲**；時間**當地 12:00**；預設駕駛艙視角，量測時**不要動視角和滑鼠**；畫質設定照平常，但**兩次要完全一樣**，並截圖附在回報裡。

**步驟**：
1. `xp_optimus_bypass.ini` 設 `mode=off`，開 X-Plane 載入上面的情境
2. 載入完成後等 **60 秒**，再什麼都不碰等 **60 秒**
3. 關掉 X-Plane，把 `xp_optimus_bypass.log` 另存一份（每次啟動都會重寫），例如 `log_off.txt`
4. 改成 `mode=auto`，重複 1～3，另存成 `log_on.txt`
5. 每份 log 只取**第二分鐘**的 `FPS` 數值，回報**中位數**，並記下那一分鐘裡 `frames over 50 ms` 的最大值

**回報**：在 GitHub 開一個「Benchmark result」issue，附上筆電型號、CPU、顯卡、NVIDIA 與 Intel 驅動版本、Windows 版本、螢幕解析度與更新率、兩個 FPS 中位數、兩份 log 和畫質設定截圖。沒有效果的結果一樣很有用。
