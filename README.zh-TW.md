# XP Optimus Bypass

給 Windows Optimus 筆電上 X-Plane 12 用的 Vulkan layer。這類筆電由 NVIDIA GeForce 顯示卡算畫面、由 Intel 內顯接螢幕；這個 layer 把算好的畫面直接交給 Intel 內顯顯示，繞過 NVIDIA 內建的 Optimus 輸出路徑，讓 FPS 大幅提高。

[English](README.md)

## 下載

**目前版本：[0.9.3.2](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.3.2)**：0.9.3 的相容性大改版，layer 裡所有等待都有時間上限，修正 Vulkan 規範細節，並加入可重設的匿名回報 ID，已在作者的 ASUS TUF F15（GTX 1650 + UHD Graphics，Windows 11）上實測。其他筆電還沒測過，匿名回報（見下方）會顯示在哪些電腦上能用。

下載 **[XP_Optimus_Bypass_v0.9.3.2.zip](https://github.com/jjjay0826/xp-optimus-bypass/releases/download/v0.9.3.2/XP_Optimus_Bypass_v0.9.3.2.zip)**。GitHub 自動產生的「Source code」壓縮檔是給開發者的。舊版（[0.9.3.1](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.3.1)、[0.9.3](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.3)、[0.9.2](https://github.com/jjjay0826/xp-optimus-bypass/releases/tag/v0.9.2)）仍可下載。

[所有版本](https://github.com/jjjay0826/xp-optimus-bypass/releases) · [安裝程式變更紀錄](installer/CHANGELOG.md) · [快速說明（英中）](installer/README.txt) · [0.9.3 規劃](docs/PLAN-0.9.3.md)

## 0.9.3.2 新功能

- `vkAcquireNextImage(2)KHR` 照 X-Plane 給的等待時間處理；一次送出多個 swapchain 時不會把 layer 的 handle 交給驅動；回報真實的螢幕更新率。
- `measure_driver=1` 會把每幀拆成等 GPU、呼叫驅動、X-Plane 自己的 CPU 計算三塊。
- 安裝程式：匿名回報加上可重設的隨機安裝 ID；X-Plane 啟動較慢（例如 Steam 啟動選項）時不會太早跳警告；只解除本套件自己檔案的下載標記。

## 0.9.3.1 新功能

- Intel 輸出最多等 2 秒拿到空的 back buffer，等不到就切到 native，DXGI Present 不會再卡住；NVIDIA 的等待超過 3 秒會記錄在 log，GPU 被重置時會直接返回。

## 0.9.3 新功能

- **Intel 改用 DXGI flip 模式輸出（D3D12）**：Intel 內顯直接讀取共享記憶體裡的畫面，透過 flip 模式顯示。因為 NVIDIA 的輸出也是 flip 模式，切到 NVIDIA 原生輸出再切回來，畫面不會再停住（舊版 Intel Vulkan 驅動會）。`intel_output=vulkan` 可改回 0.9.2 的做法。
- **Intel 驅動卡住也不會讓 X-Plane 卡死**：等待 Intel 有時間上限，超過就改用原生輸出；超過 3 秒沒有新畫面時，看門狗會在 log 寫 `[hang]`。
- **Steam 版直接啟動**：在 `X-Plane.exe` 旁邊放 `steam_appid.txt`，Steam 就不會重新啟動 X-Plane，導致 layer 沒載入。
- **在 layer 內部切換路徑**：X-Plane 一律畫進 layer 的影像，每一幀再決定走 Intel 內顯（*bypass*），還是交給 layer 自己建立的 NVIDIA 輸出（*native*）。視窗移到 NVIDIA 接的螢幕、Intel 出錯、或下面的自動比較，都會直接切換，不需要 X-Plane 重建輸出。
- **依實測選路徑**：`path=auto` 時，第一次在這台電腦上飛行，layer 會在**飛行中**（載入畫面結束 30 秒後，選單裡不會）比較 native 和 bypass，順序是 native／bypass／native／bypass，各 300 幀。bypass 的每幀中位數要快 5% 以上、P95 不能差超過 10% 才會選它。結果依顯示卡、驅動和螢幕存在 `xpob_path_cache.txt`。比較大約 1 分鐘，期間 FPS 會兩度降到約一半，切換時畫面會頓一下；安裝時會先提醒。
- **相容性**：用 LUID 找出接 X-Plane 螢幕的那張 Intel 內顯；顯示卡不能匯入共享記憶體時改用 CPU 複製；沒有多餘的 queue 時改用 transfer queue；BGRA8 以外的格式會先轉換，大小不同時會縮放；只提供 bypass 處理得了的 SDR 格式給 X-Plane。
- **診斷**：`[diag]` 會記錄所有顯示卡、LUID、擴充功能、queue、螢幕、swapchain 請求、已登記的 Vulkan layer 和載入的 overlay DLL。每 300 幀記錄一次每幀時間的 **P50／P95／P99**、最大值、超過 50 ms 的幀數，以及目前路徑的 GPU 複製、CPU 複製、等待、取得和輸出時間。
- ini 的**測試開關**可以讓一台筆電跑遍每條路徑（見「設定」）。
- **安裝程式**：全域模式改成進階選項並會警告反作弊；偵測到沒解壓縮就執行、或 PowerShell 受限時會提示；`installer-error.log` 記錄更完整；zip 裡多了不需要 PowerShell 的 `fallback` 資料夾；不會再留下空的 cmd／PowerShell 視窗。
- **匿名相容性回報**：預設開啟，可在管理視窗關閉（見下方）。

## 需求

- Windows 10/11，有 Windows PowerShell 5.1 的桌面環境。PowerShell 受限時可用 `fallback` 資料夾。
- X-Plane 12 以 Vulkan 在 NVIDIA GeForce 顯示卡上執行。
- X-Plane 所在的螢幕由 Intel 內顯驅動，通常是沒有 MUX 開關、或 MUX 設成混合模式的 Optimus 筆電。

`mode=auto` 時，偵測不到這種顯示卡／螢幕配置就不會啟用 bypass。桌機、設成獨顯直連的筆電、或直接接在 NVIDIA 顯示卡上的外接螢幕都不是適用對象。

最初的研究是在 Intel 第 10 代筆電上做的，其他硬體（包括較新的混合顯示配置）效果因機器而異。這個實作不支援 AMD 內顯、AMD／Intel 獨立顯示卡、Linux 和 macOS。

## 安裝

1. 完整解壓縮 `XP_Optimus_Bypass_v0.9.3.2.zip`，BAT 檔和 `app`、`docs`、`fallback` 資料夾要放在一起。
2. 關閉 X-Plane，執行 **`XP_Optimus_Bypass.bat`**。
3. 確認 **X-Plane 的安裝位置**：從清單選、按「瀏覽」，或輸入 `X-Plane.exe` 或它所在資料夾的路徑。
4. 選一個資料夾安裝 **XP Optimus Bypass**，預設是 `%LOCALAPPDATA%\Programs\XP Optimus Bypass`，要選你的 Windows 帳號能寫入的位置。
5. 選啟用模式、桌面捷徑、安裝後啟動和回報選項，按 **安裝**。安裝前會先跳出提示，說明匿名回報預設開啟，以及第一次飛行時的自動比較。

安裝程式打不開時（例如 PowerShell 受限），改用 `fallback` 資料夾：先執行 `install.bat`，再用 `start_xplane.bat` 啟動；`uninstall.bat` 可移除。

| 啟用模式 | 怎麼啟動 X-Plane |
|---|---|
| **僅 X-Plane（建議）** | 用管理視窗的 **啟動 X-Plane** 或專用捷徑，它們會先啟用 layer 再開遊戲；一般方式開 X-Plane 不會啟用。Steam 版會在 `X-Plane.exe` 旁邊加 `steam_appid.txt`，避免 Steam 重開遊戲而沒載入 layer（Steam 要先開著；解除安裝時會刪除）。 |
| **全域** | 照平常方式開 X-Plane。目前 Windows 帳號下的 Vulkan 程式都會載入 DLL，但 bypass 只在 `X-Plane.exe` 裡作用。 |

自動尋找 X-Plane 會在背景執行；常見位置找不到時會搜尋本機硬碟，可能要幾分鐘。網路磁碟或隨身碟上的安裝可以手動選。

安裝完成後可以刪掉解壓縮的下載資料夾，但安裝資料夾要留著。要換安裝位置，先解除安裝再重新安裝；想保留的設定和 log 請先備份。

## Log 與診斷

Log 寫在**你選的 XP Optimus Bypass 安裝資料夾第一層**，不是下載資料夾：

| 檔案 | 內容 |
|---|---|
| `xp_optimus_bypass.log` | DLL 初始化、bypass 狀態、系統資訊和效能統計 |
| `launcher-diagnostics.log` | 啟動請求、layer 登記檢查，以及沒收到 DLL 輸出時的診斷 |

在管理視窗按 **紀錄與說明** 可開啟每秒更新的檢視視窗。`stats=1` 時每 **300 幀**記錄一次 FPS 和時間，所以間隔會隨 FPS 變動。每次啟動 X-Plane 會重建 log，需要保留請先另存。

bypass 啟用時會看到：

```text
XP Optimus Bypass 0.9.3 (build ...) loaded in X-Plane.exe | ...
[diag] Intel output: DXGI flip model through D3D12 on Intel(R) ... Graphics | ...
ACTIVE: X-Plane ... -> GeForce -> host memory -> Intel -> screen
```

`build` 編號代表是哪一個 DLL。`[hang]` 表示好幾秒沒有新畫面，並會註明是 X-Plane 自己在忙（例如載入）還是 layer 在等什麼；`[slow]` 表示某一次輸出特別慢。

只有 FPS 紀錄不代表 bypass 有作用，`mode=off` 時也會記錄 FPS。沒有 DLL 輸出時，先確認看的是安裝資料夾裡的 log、僅 X-Plane 模式有用專用啟動方式，再看 `launcher-diagnostics.log`。

## 設定

編輯**安裝資料夾裡的 `app/xp_optimus_bypass.ini`**，改完重開 X-Plane。

| 設定 | 值 |
|---|---|
| `mode` | `auto`：偵測到需要的配置才啟用；`on`：強制嘗試；`off`：不用 bypass，但 `stats=1` 時仍記錄 FPS |
| `vsync` | `0`：預設，不限 FPS，可能畫面撕裂；`1`：垂直同步 |
| `stats` | `1`：預設，每 300 幀記錄 FPS 和時間；`0`：不記錄 |
| `path` | `auto`：預設，視窗在 Intel 接的螢幕時用 bypass（依實測結果），否則用 native；`bypass`／`native`：強制其中一條；`cpu-copy`：bypass 加 CPU 複製（測試用） |
| `probe` | `1`：預設，每種配置實測比較一次並保留較快的（需要 DXGI 輸出）；`0`：在 Intel 螢幕上一律用 bypass。刪掉 `xpob_path_cache.txt` 可重新比較 |
| `intel_output` | `auto`：預設，Intel 透過 DXGI flip 模式（D3D12）輸出，不行才用 Vulkan；`dxgi`；`vulkan`：0.9.2 的做法（同一個視窗用過 native 之後無法切回 bypass） |
| `measure_driver` | `1`：記錄 X-Plane 主執行緒每幀在驅動裡各類呼叫花的時間（`[drv]`，診斷用）；預設 `0` |
| `test_*` | 測試開關，平常設 `0`：`test_force_blit`、`test_force_cpu_copy`、`test_no_extra_queue`、`test_fail_intel_after=N`（N 幀後模擬 Intel 出錯）、`test_bypass_after_native` |

ini 的 `mode` 控制 bypass 行為，跟安裝時選的「僅 X-Plane／全域」不同，後者控制 DLL 怎麼載入。

## 解除安裝

關閉 X-Plane，開管理視窗按 **解除安裝**，或到 Windows 設定 → 應用程式移除 **XP Optimus Bypass**。

DLL 還在使用中時，先關閉相關的 Vulkan 程式再試一次。之後可以刪掉下載資料夾，以及另外的安裝暫存 `%LOCALAPPDATA%\XP Optimus Bypass Installer`。

## 效能結果

以下是作者的 ASUS TUF F15（i5-10300H、GTX 1650、無 MUX）上的測量，**不保證其他電腦也一樣**。

| 測試 | 未用 layer／native | 使用 layer（bypass） |
|---|---|---|
| Baron 58，KSEA，0.9.3 自動比較（log 的 probe 紀錄，每幀中位數） | 43.6～46.9 ms（約 21～23 FPS） | 22.2～24.7 ms（約 40～45 FPS） |
| Baron 58，KSEA 16L，晴天，當地中午（`0.9.1-beta`） | 中位數 21.3 FPS | 中位數 34.9 FPS |
| 同一 KSEA 測試，每 300 幀中超過 50 ms 的幀數 | 37～63 | 0～4 |
| Baron 58，RCYU，當地午夜（早期測試版） | 約 20 FPS | 40～45 FPS |
| A330，RCYU，當地午夜（`0.9.0-beta`） | 低於 19 FPS | 36～41 FPS |

RCYU 夜間比 KSEA 中午輕，兩者不能直接比較。啟用 layer 後，剩下的瓶頸主要是 X-Plane 主執行緒的 CPU 運算，在空中有時則是 GTX 1650 本身。

要做可比較的測量，請照[測試流程](installer/docs/BENCHMARK.md)，並保留 `mode=off` 和 `mode=auto` 兩份 log。

## 運作原理

1. X-Plane 把畫面畫進 layer 管理的影像。
2. 背景執行緒用 `VK_EXT_external_memory_host`，把 NVIDIA 畫好的畫面複製到固定的系統記憶體。
3. Intel 內顯透過 D3D12 直接讀那塊記憶體，用 DXGI flip 模式的 swapchain 顯示在 X-Plane 的視窗上（0.9.3；`intel_output=vulkan` 則用 0.9.2 的 Intel Vulkan swapchain）。因為 NVIDIA 自己的輸出也是 flip 模式，layer 隨時可以在兩者之間切換。

Intel 端出錯時，layer 會在內部改用 NVIDIA 原生輸出直到這次結束，FPS 會變回原本的水準，但 X-Plane 繼續運作。

原始分析見[測量紀錄](docs/measurements.md)和[研究筆記](docs/investigation_zh-TW.md)。

## 匿名相容性回報

為了知道 layer 在哪些筆電上能用，啟動器會在 **X-Plane 關閉後**送出一份小小的匿名回報。它**預設開啟**，安裝時會提醒，隨時可在管理視窗取消勾選「傳送匿名相容性回報」；「查看會傳送什麼」可看實際內容。

- **會送出**：一個隨機的匿名安裝 ID（見下方）、layer 和安裝程式版本、Windows 版本、X-Plane 版本、顯示卡型號和驅動版本、螢幕數量、X-Plane 的螢幕是否由 Intel 驅動、選用的路徑和原因、失敗原因、各路徑的 FPS 中位數和每幀時間百分位數、Vulkan overlay layer 名稱、是否用了測試開關。
- **不會送出**：檔案路徑、使用者名稱、電腦名稱、log 本身、硬體序號或 LUID。
- **匿名 ID（安裝程式 0.9.3.2 起）**：第一次產生回報時在你的電腦上隨機產生（`app\report_id.txt`），跟硬體和個人資料無關，只用來把同一份安裝的多份回報歸在一起（例如更新驅動前後比較）。回報視窗的「重設匿名 ID」可以換一個新的，解除安裝就會刪掉。公開彙整只會顯示安裝數量，不會顯示 ID。
- DLL 本身不會連網，只有啟動器會，每次一份，失敗就放棄。
- 伺服器（[`server/`](server/)）只存這些欄位和日期，不存 IP 和請求標頭。
- 依顯示卡組合彙整的結果公開在 <https://xpob-reports.jjjay0826.workers.dev/v1/summary>。

紀錄視窗也有 **產生回報** 按鈕，可以把同一份內容複製到論壇或 GitHub Issue。

## 相容性注意事項與回報問題

- `vsync=0` 可能畫面撕裂，需要時改成 `vsync=1`。
- 全域模式會讓沒有數位簽章的 DLL 載入其他 Vulkan 程式，可能和反作弊軟體衝突；這種環境請用「僅 X-Plane」模式，切換模式後重開相關程式。
- Vulkan overlay 或錄影工具可能造成相容性問題，查閃退時請先關掉。
- 目前的測試不代表能相容 VR、所有多螢幕配置、裝很多外掛的長時間飛行，或所有顯示卡和驅動組合。
- 虛擬 swapchain 的已知限制：一次送出多個 swapchain（X-Plane 不會這樣做）時，這次執行會改用一般的 NVIDIA swapchain，該次送出的擴充資訊也不會保留；不支援 Vulkan device group（`deviceMask`）；`VK_GOOGLE_display_timing` 會回報真實更新率，但沒有過去的呈現時間；NVIDIA 低延遲（Reflex）相關呼叫只對真實 swapchain 交給驅動，對 layer 的 swapchain 會直接返回。
- 如果 layer 背景執行緒裡的某個驅動呼叫一直不返回，layer 會在 30 秒後放棄：X-Plane 會收到 device lost（報錯）而不是一直卡住，layer 的物件會留著直到 X-Plane 結束。
- 回報 X-Plane 問題給 Laminar Research 前，先確認解除安裝這個 layer 後是否還會發生。

請到 [GitHub Issues](https://github.com/jjjay0826/xp-optimus-bypass/issues) 回報，附上版本、筆電型號、CPU／GPU、Windows 和驅動版本、啟用模式、相關 log 和重現步驟。效能比較請附上畫質設定和兩份測試 log。

## 發布前怎麼測試

GitHub Actions 會可重現地編譯和打包每個版本、在 Windows 上檢查安裝程式（語法、中英文字、安裝路徑安全、回報隱私），並附上建置來源證明。**但它不會跑顯示卡、Vulkan、D3D12 或 X-Plane。** 這部分是發布前在作者筆電上手動測的，結果和還沒測的項目都在 [installer/docs/TESTING.md](installer/docs/TESTING.md)。

## 驗證下載

DLL 沒有數位簽章。可以比對 ZIP 的 SHA-256（PowerShell：`Get-FileHash XP_Optimus_Bypass_v*.zip`）和 release 說明裡的是否相同；`xp_optimus_bypass.log` 第一行會顯示 DLL 版本和 build id。要檢查 DLL 本身，在**解壓縮後的資料夾第一層**執行：

```bat
certutil -hashfile app\xp_optimus_bypass.dll SHA256
```

把結果和 `SHA256SUMS.txt` 裡 `app/xp_optimus_bypass.dll` 那一行比對（前面可能有 `./`）。GitHub Actions 發布的版本另有建置來源證明：

```sh
gh attestation verify XP_Optimus_Bypass_v0.9.3.2.zip -R jjjay0826/xp-optimus-bypass
```

證明只適用於 workflow 產生的原始壓縮檔，修改或重新打包過的 zip 就不適用。

## 從原始碼編譯

用 `build.sh` 裡說明的 MinGW-w64 工具：

```sh
sh build.sh
sh package.sh 0.9.3.2
```

會產生 `XP_Optimus_Bypass_v0.9.3.2.zip`。

- Vulkan layer 原始碼：[`src/xp_optimus_bypass.cpp`](src/xp_optimus_bypass.cpp)
- 安裝程式原始碼和文件：[`installer/`](installer/)
- 建置與發布流程：[`.github/workflows/build.yml`](.github/workflows/build.yml)

## 致謝與授權

最初的研究和 layer 實作由作者在 Anthropic 的 Claude 協助下完成，使用了 Intel VTune、NVIDIA Nsight Systems、PresentMon 和實驗用的 layer。

[MIT 授權](LICENSE)。與 Laminar Research、NVIDIA、Intel 無關。
