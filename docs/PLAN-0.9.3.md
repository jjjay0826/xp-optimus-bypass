# 0.9.3 實作規劃：相容性與診斷

目標：在 README 列出的條件都符合的電腦上，盡量讓 bypass 正常運作。不能運作的時候，要自動退回 NVIDIA 原本的路徑，而且 log 要寫清楚原因。

不在 0.9.3 範圍內：AMD 內顯支援（0.10）、X-Plane 外掛開關（0.11）、自動回報伺服器。0.9.3 的 LUID 判斷和診斷 log 會先替這些功能打好基礎。

---

## 問題清單與做法

### A. 診斷 log 加強（最先做，其他項目都靠它驗證）

啟動時一次記錄以下項目，每一行開頭加 `[diag]`：

| 項目 | 怎麼取得 |
|---|---|
| 所有 Vulkan 實體裝置：名稱、廠商、驅動、Vulkan 版本、**LUID** | `vkGetPhysicalDeviceProperties2` + `VkPhysicalDeviceIDProperties` |
| 每個裝置是否支援 `VK_EXT_external_memory_host`、`minImportedHostPointerAlignment` | `vkEnumerateDeviceExtensionProperties`、`VkPhysicalDeviceExternalMemoryHostPropertiesEXT` |
| 所有螢幕：解析度、更新率、DPI 縮放、由哪張顯示卡驅動（LUID） | DXGI `EnumAdapters1` → `EnumOutputs` → `DXGI_OUTPUT_DESC.Monitor` |
| X-Plane 視窗在哪個螢幕上 | `MonitorFromWindow` |
| X-Plane 要求的 swapchain：格式、色彩空間、大小、usage、present mode、影像數 | `XP_CreateSwapchainKHR` 的參數 |
| Intel surface 支援的格式和大小 | `vkGetPhysicalDeviceSurfaceFormatsKHR` / `Capabilities` |
| NVIDIA graphics queue family 的 queue 數，以及 X-Plane 用了幾個 | `vkGetPhysicalDeviceQueueFamilyProperties` + `VkDeviceCreateInfo` |
| 已登記的 implicit layer，以及實際載入的已知 overlay DLL | 登錄檔 `ImplicitLayers`（HKCU + HKLM）、`GetModuleHandle` 查已知檔名 |
| 最後選擇的路徑和原因 | 例如 `path=direct-import`、`path=blit+import`、`path=cpu-copy`、`path=normal (reason: ...)` |

路徑中的使用者名稱一律換成 `%USERPROFILE%`，讓 log 可以直接公開貼上。

**測試用開關**（寫在 ini 裡，平常不用；讓只有一台筆電也能測每條路徑）：
```
test_force_blit=1        ; 即使格式、大小都相符也走 blit
test_force_cpu_copy=1    ; 不使用 external_memory_host
test_fail_intel_after=600 ; 第 600 幀模擬 Intel 失敗，驗證自動回復
test_no_extra_queue=1    ; 假裝沒有多餘的 queue，驗證備用 queue
```

### B. 用 LUID 選擇 Intel 裝置（取代「第一個 Intel 裝置」）

- 現況：`intelCreate` 選第一個 `vendorID == 0x8086` 的裝置；判斷螢幕時用 `EnumDisplayDevices` 的 `VEN_8086` 字串。
- 新做法：
  1. 用 DXGI 找到 X-Plane 視窗所在螢幕的**顯示卡 LUID**。
  2. 在 Vulkan 裝置中找 `deviceLUID` 相同的那張，再確認它能 present 到這個視窗（`vkGetPhysicalDeviceSurfaceSupportKHR`）。
  3. 判斷條件改成「**顯示用的卡 ≠ 渲染用的卡**」，不再寫死 Intel。0.9.3 仍然只啟用 Intel，但 0.10 支援 AMD 時只要放寬廠商限制。
- 找不到 LUID 時（例如舊驅動）退回目前的做法，並寫進 log。

### C. 不支援 `VK_EXT_external_memory_host` 時的備用路徑

- 現況：NVIDIA 或 Intel 任一邊不支援，就直接停用 bypass。
- 新做法：分成三種路徑，依序嘗試：
  1. `direct-import`：兩邊都匯入同一塊固定記憶體（目前的做法，最快）。
  2. `half-import`：只有一邊支援，就由那邊匯入，另一邊用一般的可映射記憶體，工作執行緒用 `memcpy` 搬一次。
  3. `cpu-copy`：兩邊都用一般的可映射記憶體，工作執行緒 `memcpy`。1080p 每幀大約 8 MB，估計 1–3 ms。
- 匯入前先檢查 `minImportedHostPointerAlignment`，配置的大小和位址都要對齊（目前固定用 64 KB，大部分驅動夠用，但要檢查而不是假設）。

### D. 沒有多餘 queue 時的備用做法

- 現況：graphics family 沒有空的 queue 就停用。
- 新做法：改用 NVIDIA 的**專用 transfer queue family**。因為影像是 `EXCLUSIVE` 模式，要加 queue family ownership transfer（release / acquire barrier）。
- 兩者都沒有時才停用，並寫進 log。

### E. Swapchain 格式與影像大小

- 現況：每像素固定當成 4 bytes；Intel 端只找 BGRA8；大小不同時只請 X-Plane 改大小，複製時取比較小的範圍。
- 問題：HDR 或 16-bit 格式會讓記憶體大小算錯（可能越界）；大小一直對不上時畫面會錯位。
- 新做法：
  1. **攔截 `vkGetPhysicalDeviceSurfaceFormatsKHR`**：當視窗在 Intel 螢幕上時，只回報 Intel 也能顯示的格式（交集）。X-Plane 一開始就不會選到我們處理不了的格式（例如 HDR 色彩空間）。
  2. 如果最後的格式仍不是 BGRA8，或大小和 Intel swapchain 不同：在 GeForce 端用 `vkCmdBlitImage` 把畫面**轉成 BGRA8 並縮放**到 Intel 的大小，再複製到共享記憶體。先確認來源格式支援 `BLIT_SRC`。
  3. 格式和大小都相符時維持直接複製（不多花時間）。
  4. 共享記憶體大小改成依「實際寫入的格式和大小」計算，不再寫死 `×4`。
  5. 不支援 blit 的格式：退回 NVIDIA 原路徑，並寫 log。

### F. 外接螢幕、MUX 與多螢幕

- 現況：只在建立裝置和 swapchain 時判斷一次。如果啟動時沒有 Intel 螢幕，連多的 queue 都不會準備，之後也無法再啟用。
- 新做法：
  1. 只要系統裡有 Intel GPU，就準備好多的 queue（成本很低），之後才能隨時切換。
  2. 每 30 幀檢查一次 X-Plane 視窗所在螢幕的顯示卡 LUID，並監聽 `WM_DISPLAYCHANGE`。
  3. 需要切換的時候：
     - bypass → 原路徑：沿用 0.9.1.1 的機制，回傳 `VK_ERROR_OUT_OF_DATE_KHR`；
     - 原路徑 → bypass：在原本的 present 回傳 `VK_SUBOPTIMAL_KHR`；
     - 兩種情況都會讓 X-Plane 重建 swapchain，由 `XP_CreateSwapchainKHR` 重新決定走哪條路徑。
  4. 視窗跨兩個螢幕時，以視窗面積佔最多的螢幕為準（`MonitorFromWindow` 的預設行為）。
  5. 切換時加冷卻時間（例如 2 秒），避免拖曳視窗時來回切換。
  6. Intel 失敗的標記（`g_intelBroken`）維持一次執行中有效，不會再切回去。

### G. Overlay / 擷取工具

- 在 log 記錄已登記的 implicit layer 和已載入的已知 overlay DLL：Steam（`GameOverlayVulkanLayer64`）、RTSS（`RTSSVkLayer64`）、OBS（`graphics-hook`）、Discord、NVIDIA App、Overwolf 等。
- 實測每一種 overlay 的結果，寫進 README 的相容性表：overlay 是否顯示、OBS 是否錄得到、FPS、會不會當機。
- 如果實測發現某個 overlay 排在我們下面會出問題：log 和管理程式要提示怎麼關掉它。

### H. 安裝程式

1. 先拿到 Reddit 網友的 `installer-error.log`，找出真正的原因。
2. 常見的失敗原因，先預防：
   - 沒有解壓縮，直接在 zip 裡執行 → 偵測路徑在暫存資料夾或壓縮檔內時，提示使用者先解壓縮；
   - 下載的檔案帶有「網際網路來源」標記 → 啟動時執行 `Unblock-File`；
   - 公司或學校電腦用群組原則鎖住 PowerShell，或處於 Constrained Language Mode → 偵測到就改用備用安裝方式；
   - 防毒軟體擋下 PowerShell → 提示使用備用方式；
   - 路徑有中文或空白、資料夾在 OneDrive 裡 → 加入 CI 測試。
3. **保留不需要 PowerShell 的備用安裝方式**：0.9.1.1 的 `install.bat` / `start_xplane.bat`，放在 zip 的 `fallback` 資料夾。
4. Global 模式移到「進階」區塊，選擇時跳出反作弊警告，必須按確認才會生效。

### I. 自動回報（預設開啟，可以關閉）

**同意與透明**
- 安裝畫面有一個**預設打勾**的選項：「傳送匿名相容性回報 / Send anonymous compatibility reports」，旁邊有「查看會傳送什麼」，會顯示一份實際的範例內容。
- 管理程式裡隨時可以關閉。關閉後完全不傳，也不在背景做任何連線。
- README、release 說明和安裝畫面都寫清楚：傳什麼、傳到哪裡、怎麼關掉。

**內容（最少必要）**
- 會傳：layer 版本、Windows build、X-Plane 版本、各 GPU 型號和驅動、螢幕數量和由哪家廠商的顯示卡驅動、選擇的路徑和原因、失敗原因（如果有）、FPS 中位數和卡頓次數、測試開關狀態。
- 不傳：檔案路徑、使用者名稱、電腦名稱、log 原文、任何可以識別個人或硬體的序號（包括 LUID 本身，只傳「是否相符」）。
- 不使用持久的使用者 ID。同一台電腦的多次回報不做關聯。

**傳送方式**
- 由啟動器在 **X-Plane 關閉後** 傳送一次（HTTPS POST，JSON，大約 1 KB）。失敗就放棄，不重試、不累積。
- **DLL 本身永遠不連網。**
- 管理程式另外有「產生回報」按鈕：顯示同樣的內容（加上去除隱私的 log 摘要），可以複製到 Reddit、論壇，或開成 GitHub issue。

**伺服器**
- Cloudflare Worker + D1（免費額度），用作者自己的帳號建立。
- 不保存 IP 和 request header，只存 JSON 內容和收到的日期（精確到天）。
- 檢查格式和大小（超過 4 KB 或欄位不符就拒收），並且限制頻率。
- 公開彙整結果（各 GPU / 驅動組合的成功率、FPS 改善幅度），放在 README 的相容性表。
- Worker 的原始碼放在 repo 的 `server/`，任何人都能檢查伺服器做了什麼。

---

## 執行順序（每個里程碑都發一個測試版給你在筆電上測）

| 里程碑 | 內容 | 你要測的東西 |
|---|---|---|
| **M1** | A 診斷 log + 測試用開關 | log 的 `[diag]` 內容是否正確（GPU、LUID、螢幕、格式） |
| **M2** | B LUID 選卡 + F 螢幕切換 | 拖視窗、插拔外接螢幕（如果有）、最小化再還原；FPS 不變 |
| **M3** | E 格式與大小（blit） | `test_force_blit=1` 畫面正常、顏色正確，FPS 只掉一點；改視窗大小 |
| **M4** | C 備用記憶體路徑 + D 備用 queue | `test_force_cpu_copy=1`、`test_no_extra_queue=1` 都能運作；記錄 FPS 差異 |
| **M5** | H 安裝程式 + G overlay 偵測 | 安裝、重裝、移除、備用安裝；Steam overlay / RTSS / OBS 實測 |
| **M6** | I 自動回報（啟動器 + Cloudflare Worker） | 關掉 X-Plane 後回報有送出、內容正確；關閉選項後完全不送 |
| **發布** | 整合測試 + 更新 README / CHANGELOG | 固定測試（KSEA）off / on，FPS 不能比 0.9.2 差 |

**原則**
- 每一條新路徑失敗時，都要能退回上一條路徑，最後退回 NVIDIA 原路徑，**絕對不能讓 X-Plane 當掉**。
- 預設情況（格式、大小相符，兩邊都支援 import）的程式碼路徑和 0.9.2 相同，效能不能變差。
- `test_fail_intel_after` 用來驗證自動回復，每個里程碑都要跑一次。
