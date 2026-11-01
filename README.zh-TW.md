# XP Optimus Bypass（測試版）

一個小型 Vulkan layer。在**沒有 MUX 開關的 Optimus 筆電**上，它改由程式自己透過 Intel 內顯把畫面送上螢幕，不走 NVIDIA 內建的 Optimus 輸出路徑，可以大幅提高 X-Plane 12 的 FPS。

作者的筆電（ASUS TUF F15，i5-10300H + GTX 1650，無 MUX）：

| 機型 | 沒有 layer | 有 layer |
|---|---|---|
| Baron 58（內建） | 約 20 FPS | 40～45 FPS |
| A330（內建） | 不到 19 FPS | 36～41 FPS |

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
2. 雙擊 `install.bat`
3. 照平常開 X-Plane，打開同資料夾裡的 `xp_optimus_bypass.log`，應該會看到：
   ```
   bypass prepared (extra GeForce queue: ...)
   Intel: device ready ...
   ACTIVE: X-Plane 1920x1080 -> GeForce -> host memory -> Intel -> screen
   ```
   之後大約每 10 秒會多一行 `FPS ...`

## 移除

在**同一個資料夾**雙擊 `uninstall.bat`，再刪掉資料夾。

## 設定（`xp_optimus_bypass.ini`）

| 設定 | 值 |
|---|---|
| `mode` | `auto`（預設）：需要時才啟用 · `on`：一律嘗試 · `off`：完全不動作 |
| `vsync` | `0`（預設）：不限幀，可能撕裂 · `1`：不撕裂，FPS 上限為螢幕更新率 |
| `stats` | `1`（預設）：每 300 幀把 FPS 和各段時間寫進 log |

改完要重開 X-Plane。

## 已知問題

- `vsync=0` 時會有**畫面撕裂**（Intel 那端是畫好就送）。介意的話用 `vsync=1`
- 尚未測試：VR、多螢幕、在不同螢幕間拖曳視窗、裝了很多外掛的長時間飛行、其他顯卡與驅動
- 會掛進 Vulkan 的 overlay（Steam overlay、Overwolf、OBS 遊戲擷取、RTSS）可能不相容。遇到崩潰請先關掉它們
- X-Plane 或驅動更新後可能失效，X-Plane 異常時請先移除
- **向 Laminar Research 回報 bug 之前，請先移除這個 layer，確認沒裝也會發生。** 這不是 Laminar 做的，他們沒辦法支援

## 回報結果

請附上：筆電型號、CPU、顯卡、NVIDIA 與 Intel 驅動版本、機型與地點、同一地點 `mode=off` 和 `mode=auto` 的 FPS，以及 `xp_optimus_bypass.log`。

## 從原始碼編譯

```
./build.sh        # 用 MinGW-w64 交叉編譯（Linux/WSL）
```

只有一個原始碼檔：`src/xp_optimus_bypass.cpp`，另外需要 Khronos 的 Vulkan 標頭檔。

## 致謝

調查與程式是作者和 AI 助理（Anthropic 的 Claude）一起完成的，過程用到 Intel VTune、NVIDIA Nsight Systems、PresentMon，以及一版一版的測試 layer。完整的調查筆記在 `docs/` 裡。

MIT 授權。與 Laminar Research、NVIDIA、Intel 無關。
