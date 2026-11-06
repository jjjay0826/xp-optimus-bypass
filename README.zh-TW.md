# XP Optimus Bypass

給 **Windows Optimus 筆電上 X-Plane 12** 用的免費 Vulkan layer：NVIDIA 顯示卡算畫面、Intel 內顯接螢幕。這類筆電上
NVIDIA 把畫面交給 Intel 螢幕的過程很慢，layer 改成自己把每一幀交給 Intel 內顯顯示。作者的 ASUS TUF F15（GTX 1650）
在 KSEA：**約 22 → 42 FPS**，卡頓也少很多。

[English](README.md) · [詳細說明](docs/DETAILS.zh-TW.md) · [變更紀錄](CHANGELOG.md) · [所有版本](https://github.com/jjjay0826/xp-optimus-bypass/releases)

## 下載

**[XP_Optimus_Bypass_v0.9.3.2.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.3.2/XP_Optimus_Bypass_v0.9.3.2.zip)**
（[版本說明](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.3.2)）。目前只在作者的筆電上測過，歡迎用其他筆電幫忙測試。

## 需求

- Windows 10/11，X-Plane 12（Vulkan）在 NVIDIA GeForce 顯示卡上執行。
- X-Plane 的螢幕由 Intel 內顯驅動：沒有 MUX 開關、或 MUX 設成混合模式的 Optimus 筆電。
- 不適用桌機、獨顯直連模式、接在 NVIDIA 顯示卡上的螢幕、AMD 顯示卡、Linux 或 macOS；這些情況下 layer 不會啟用。

## 安裝與使用

1. 完整解壓縮 zip，執行 **`XP_Optimus_Bypass.bat`**。
2. 確認 X-Plane 路徑和安裝資料夾，按 **安裝**。
3. 用管理視窗的 **啟動 X-Plane** 或桌面捷徑開遊戲（Steam 版要先開 Steam）。在建議的「僅 X-Plane」模式下，用一般方式開 X-Plane 不會載入 layer。
4. 每台電腦第一次飛行時，會花約 1 分鐘比較兩種輸出方式（會短暫卡頓、FPS 兩度掉一半），之後記住較快的那個。

移除：管理視窗按 **解除安裝**。PowerShell 受限時改用 zip 裡的 `fallback` 資料夾。設定在安裝資料夾的
`app\xp_optimus_bypass.ini`，說明見[詳細說明](docs/DETAILS.zh-TW.md#設定)。

## 效能

作者的 ASUS TUF F15（i5-10300H、GTX 1650、無 MUX），Baron 58，KSEA。不保證其他筆電也一樣。

| | 不用 layer | 使用 layer |
|---|---|---|
| 每幀時間中位數（layer 自己量的） | 43.6～46.9 ms（約 21～23 FPS） | 22.2～24.7 ms（約 40～45 FPS） |
| 每 300 幀中超過 50 ms 的幀數 | 37～63 | 0～4 |

使用 layer 後，瓶頸是 GTX 1650 本身（Nsight Systems：每幀約 86% 的時間在工作）。可比較的測量方法見
[測試流程](installer/docs/BENCHMARK.md)，分析見[測量紀錄](docs/measurements.md)。

## 匿名回報

X-Plane 關閉後，啟動器會送出一份小回報：顯示卡型號、驅動、Windows／X-Plane 版本、FPS、用了哪種輸出方式，以及在你電腦上
隨機產生、可重設的 ID。不含姓名、路徑、IP 或硬體編號。預設開啟，可在管理視窗關閉或查看實際內容。公開結果：
<https://xpob-reports.jjjay0826.workers.dev/v1/summary>　·　[完整說明](docs/DETAILS.zh-TW.md#匿名相容性回報)

## 問題與限制

- layer 裡每個等待都有時間上限；Intel 端出錯時會改用 NVIDIA 原本的輸出，X-Plane 繼續運作。驅動卡死時 X-Plane 會收到錯誤，而不是一直卡住。
- 還沒測過：其他筆電、外接或多螢幕、HDR、VR、全域模式。已知限制見[詳細說明](docs/DETAILS.zh-TW.md#相容性注意事項與回報問題)。
- 有問題請到 [GitHub Issues](https://github.com/jjjay0826/xp-optimus-bypass/issues)，附上筆電型號和 `xp_optimus_bypass.log`（管理視窗 → **紀錄與說明** → **產生回報** 可以複製整理好的回報）。
- DLL 沒有數位簽章：[如何驗證下載檔](docs/DETAILS.zh-TW.md#驗證下載)。

## 授權

[MIT](LICENSE)。由作者在 Anthropic 的 Claude 協助下開發。與 Laminar Research、NVIDIA、Intel 無關。
