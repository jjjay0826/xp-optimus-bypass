# XP Optimus Bypass（測試版）

一個小型 Vulkan layer。在**沒有 MUX 開關的 Optimus 筆電**上，它改由程式自己透過 Intel 內顯把畫面送上螢幕，不走 NVIDIA 內建的 Optimus 輸出路徑，可以大幅提高 X-Plane 12 的 FPS。

### 下載

**目前版本：0.9.1-beta** → **[下載 XP_Optimus_Bypass_v0.9.1-beta.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.1-beta/XP_Optimus_Bypass_v0.9.1-beta.zip)**（照 zip 裡的說明安裝）。
**測試用預先發行版：0.9.1.1-beta** → **[下載 XP_Optimus_Bypass_v0.9.1.1-beta.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.1.1-beta/XP_Optimus_Bypass_v0.9.1.1-beta.zip)**：只給 X-Plane 的安裝方式、出錯時自動回復而不當機。還沒在 Windows 上測過；下方的[安裝](#安裝)步驟是這一版的。
各版本變更：[CHANGELOG](CHANGELOG.md) · 校驗碼和舊版本：[Releases](https://github.com/jjjay0826/xp-optimus-bypass/releases) · [English](README.md)

作者的筆電（ASUS TUF F15，i5-10300H + GTX 1650，無 MUX）：

| 測試 | 沒有 layer | 有 layer |
|---|---|---|
| **[固定測試](BENCHMARK.md)：Baron 58 停在 KSEA 16L（0.9.1-beta）** | **21.3 FPS** | **34.9 FPS** |
| 同一測試中超過 50 ms 的卡頓幀（每 300 幀） | 37～63 | 0～4 |
| Baron 58，RCYU 花蓮、當地午夜（正式發布前的測試版） | 約 20 FPS | 40～45 FPS |
| A330，RCYU 花蓮、當地午夜（0.9.0-beta） | 不到 19 FPS | 36～41 FPS |

固定測試的數值是 log 裡各 300 幀區段的中位數。RCYU 那兩列是小機場、夜間，場景比中午的 KSEA 輕，不能直接比較。KSEA（西雅圖）周圍物件很多、很吃 CPU；開啟 layer 後，那裡剩下的瓶頸是 X-Plane 自己的 CPU 計算（把 FSR 或物件密度調低只多 2～3 FPS）。

> 這是只在**一台**筆電上測過的測試版。不管有沒有效，都歡迎附上 log 回報。

## 適用對象

以下條件**全部**符合才可能有幫助：

- Windows 筆電、X-Plane 12（Vulkan）
- X-Plane 用 NVIDIA GeForce 繪圖
- 內建螢幕接在 **Intel** 內顯上（Optimus、沒有 MUX，或 MUX 設成混合模式）
- 特別是 Intel **10 代以前**，這些不支援 Windows 11 的 CASO

桌機、MUX 切到獨顯的筆電，或 X-Plane 視窗在接獨顯的外接螢幕上時，沒有幫助（`mode=auto` 會自動停用）。Intel 11 代以後有 CASO，效果未知。

不支援：AMD 內顯、AMD／Intel 獨顯、Linux、macOS。

## 為什麼有效

這類筆電的每一幀都要走：獨顯 → 共用記憶體 → Intel 內顯 → 螢幕。在測試的筆電上，飛行時 NVIDIA 自己這條路每幀要花大約 **47 毫秒**（主選單時只要 0.5 毫秒），所以不管怎麼調設定，X-Plane 都超不過大約 21 FPS。把送出畫面的動作移到其他執行緒也沒用，這條路本身每秒就只能送大約 21 張。

這個 layer 取代了這條路：

1. X-Plane 畫進 layer 自己準備的影像（不會拿到真正的 NVIDIA swapchain）
2. 另一條執行緒把畫好的每一幀，從獨顯複製到釘住的系統記憶體（`VK_EXT_external_memory_host`）
3. layer 在 Intel 內顯上開一個獨立的 Vulkan 裝置，把畫面複製到它自己在 X-Plane 視窗上的 swapchain 並送出

獨顯複製大約 1～2 毫秒，Intel 送出大約 0.02 毫秒。之後瓶頸變成獨顯本身的繪圖速度，所以調畫質會重新有效。

所有量測數據（PresentMon、VTune、Nsight，以及每一版測試 layer 的各函式計時）在 [`docs/measurements.md`](docs/measurements.md)（英文），完整調查過程在 [`docs/investigation_zh-TW.md`](docs/investigation_zh-TW.md)。

## 安裝

1. 解壓到一個固定的資料夾，例如 `C:\XP_Optimus_Bypass`。**之後不要移動這個資料夾**，安裝時登記的就是這個路徑
2. 雙擊 `install.bat`，選擇：
   - **1 – 只給 X-Plane（建議）**：其他 Vulkan 程式完全不會載入這個 layer。之後用這個資料夾裡的 `start_xplane.bat` 開 X-Plane。第一次執行時會自動找 Steam 預設的安裝位置，找不到就請你把 `X-Plane.exe` 拖進視窗。用其他方式開 X-Plane 就不會有 layer
   - **2 – 所有 Vulkan 程式**（0.9.1 的做法）：照平常開 X-Plane。所有 Vulkan 程式都會載入這個 DLL，但只在 `X-Plane.exe` 裡作用（見[已知問題](#已知問題)）
3. 開 X-Plane，打開同資料夾裡的 `xp_optimus_bypass.log`，應該會看到：
   ```
   bypass prepared (extra GeForce queue: ...)
   Intel: device ready ...
   ACTIVE: X-Plane 1920x1080 -> GeForce -> host memory -> Intel -> screen
   ```
   之後大約每 10 秒會多一行 `FPS ...`。選 1 的話，如果完全沒有 log，代表 layer 沒有被載入，請確認是用 `start_xplane.bat` 開的

   *Steam 啟動選項（未測試）*：也可以不用 `start_xplane.bat`，改在 X-Plane 的 Steam 啟動選項填入
   `cmd /c "set XP_OPTIMUS_BYPASS_ENABLE=1&& %command%"`。log 裡出現 `started with XP_OPTIMUS_BYPASS_ENABLE=1 ...: yes` 就代表成功

**檢查下載的檔案（選擇性）**：這個 DLL 沒有數位簽章，有些防毒軟體或 Windows SmartScreen 可能會跳警告。可以確認它和公開的版本一模一樣：在資料夾開命令提示字元，輸入
```
certutil -hashfile xp_optimus_bypass.dll SHA256
```
結果要跟 `SHA256SUMS.txt`（以及 Release 說明）裡的值相同。也可以用 `src/` 自己編譯（見下方）。

## 移除

雙擊 `uninstall.bat`，再刪掉資料夾。0.9.1.1 起會移除所有 XP Optimus Bypass 的登記，包括從其他資料夾安裝的。

## 設定（`xp_optimus_bypass.ini`）

| 設定 | 值 |
|---|---|
| `mode` | `auto`（預設）：需要時才啟用 · `on`：一律嘗試 · `off`：不啟用（0.9.1 起仍會記錄 FPS，方便比較） |
| `vsync` | `0`（預設）：不限幀，可能撕裂 · `1`：不撕裂，FPS 上限為螢幕更新率 |
| `stats` | `1`（預設）：每 300 幀把 FPS 和各段時間寫進 log |

改完要重開 X-Plane。

## 已知問題

- DLL **沒有數位簽章**，防毒軟體或 SmartScreen 可能會警告，請照上面的方法檢查 SHA-256
- `vsync=0` 時會有**畫面撕裂**（Intel 那端是畫好就送）。介意的話用 `vsync=1`
- 尚未測試：VR、多螢幕、在不同螢幕間拖曳視窗、裝了很多外掛的長時間飛行、其他顯卡與驅動
- 安裝時選 **2（所有 Vulkan 程式）** 的話，你 Windows 帳號下的所有 Vulkan 程式都會載入這個 DLL。它在 `X-Plane.exe` 以外什麼都不做，但有**反作弊**的線上遊戲可能不接受沒簽章的 DLL。請改選 1，或在用 Vulkan 模式玩這類遊戲前先執行 `uninstall.bat`。選 1 的話，只有 `start_xplane.bat` 開的程式（X-Plane 和它自己開的程式）會載入
- 會掛進 Vulkan 的 overlay（Steam overlay、Overwolf、OBS 遊戲擷取、RTSS）可能不相容。遇到崩潰請先關掉它們
- 如果飛行中 Intel 那端出錯，layer 會在這次執行剩下的時間改走 NVIDIA 原本的路徑（log 會寫 `switching back to NVIDIA's normal path`）：FPS 會掉回去，但 X-Plane 不會當掉
- X-Plane 或驅動更新後可能失效，X-Plane 異常時請先移除
- **向 Laminar Research 回報 bug 之前，請先移除這個 layer，確認沒裝也會發生。** 這不是 Laminar 做的，他們沒辦法支援

## 回報結果

請照 [BENCHMARK.md](BENCHMARK.md) 的流程（約 10 分鐘：同一情境分別用 `mode=off` 和 `mode=auto`），在 GitHub 開一個 **Benchmark result** issue 並附上兩份 log。遇到問題請開 **Bug report** issue。各版本的變更在 [CHANGELOG.md](CHANGELOG.md)。

## 未來可能的方向

- **其他 Vulkan 遊戲**：這個 layer 大部分跟遊戲無關（目前只在 `X-Plane.exe` 裡作用）。還不知道其他遊戲是不是也有同樣慢的 Optimus 輸出路徑；如果有，可能會推出通用版（逐款遊戲手動開啟）
- **DirectX 9～11 遊戲**：也許能搭配 DXVK（把 DirectX 轉成 Vulkan）一起使用，尚未測試
- 有反作弊的線上遊戲不會支援

沒有保證也沒有時間表，取決於測試結果。如果你知道其他 Vulkan 遊戲在這類筆電上也卡在低 FPS，歡迎開 issue 告訴我。

## 從原始碼編譯

```
./build.sh                     # 用 MinGW-w64 交叉編譯（Ubuntu 24.04 / WSL）
./package.sh 0.9.1.1-beta      # 打包 release zip
```

Release 的 zip 是 GitHub Actions 直接從這個 repo 編譯出來的（[build.yml](.github/workflows/build.yml)），SHA-256 會寫在 release 說明裡。編譯結果可以重現：用同樣的工具（Ubuntu 24.04、MinGW-w64 GCC 13 win32、Vulkan-Headers v1.4.367）執行 `build.sh` 會得到一模一樣的 DLL。要確認 zip 是 GitHub Actions 從這個 repo 編出來的：`gh attestation verify XP_Optimus_Bypass_v<版本>.zip -R jjjay0826/xp-optimus-bypass`。

只有一個原始碼檔：`src/xp_optimus_bypass.cpp`，另外需要 Khronos 的 Vulkan 標頭檔。

## 致謝

調查與程式是作者和 AI 助理（Anthropic 的 Claude）一起完成的，過程用到 Intel VTune、NVIDIA Nsight Systems、PresentMon，以及一版一版的測試 layer。完整的調查筆記在 `docs/` 裡。

MIT 授權。與 Laminar Research、NVIDIA、Intel 無關。
