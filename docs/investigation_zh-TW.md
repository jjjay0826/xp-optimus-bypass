# X-Plane 12 在 Optimus 筆電上卡在約 20 FPS：瓶頸分析與全部數據

> 用途：請其他 AI 獨立檢查這份分析的推論是否成立、有沒有漏掉的可能性。
> 標記說明：**【量測】** = 工具直接量到的數字；**【推論】** = 根據數字推出來、沒有直接證據的結論；**【使用者描述】** = 使用者口頭回報、沒有保存原始數據。

---

## 1. 結論（待驗證）

在這台沒有 MUX 的 Optimus 筆電上，X-Plane 12 飛行時大約 20 FPS（一幀約 47～50 ms）。
目前的結論是：**瓶頸在「獨顯 → 內顯 → 內建螢幕」的畫面輸出路徑（NVIDIA 驅動內部）**，不在 X-Plane 的 CPU 計算、也不在畫質設定。
除了外接直連獨顯的螢幕（或換有 MUX／支援 CASO 的電腦），沒有找到使用者端能大幅改善的方法。

---

## 2. 系統環境

| 項目 | 內容 |
|---|---|
| 筆電 | ASUS TUF Gaming F15 FX506LHB（2022/3 出廠），**無 MUX**，內建螢幕接在 Intel 內顯 |
| CPU | Intel Core i5-10300H（Comet Lake-H，4 核 8 緒，L3 8 MB，CPUID family 6 model 165） |
| 獨顯 | NVIDIA GeForce GTX 1650 Laptop 4 GB（TU117，14 SM），驅動 595.97，Vulkan 1.4.329 |
| 內顯 | Intel UHD Graphics（10 代） |
| RAM | DDR4-2933 雙通道【使用者描述】 |
| 解析度 | 1920×1080（螢幕更新率未確認） |
| X-Plane | 12.4.3-r2（build 124311，Vulkan），Steam 版 |
| 外掛 | AviTab、NextATC、Skunkcrafts updater 等；X-Plane 有一個給外掛用的 OpenGL bridge device（GTX 1650） |
| 測試機型 | 多數測試用內建 Beechcraft Baron 58（測試期間停機煞車拉著，煞車音效持續播放） |
| 已載入的隱式 Vulkan layer（來自 X-Plane Log） | VK_LAYER_NV_optimus、VK_LAYER_NV_present、VK_LAYER_RTSS（被 X-Plane 停用）、VK_LAYER_OW_OVERLAY、VK_LAYER_OW_OBS_HOOK、VK_LAYER_OBS_HOOK、Steam overlay、Steam fossilize 等 |
| Queue families | 0：Graphics+Compute+Transfer+Present；1：Transfer；2：Async compute；3：Video decode；4：Video encode |

---

## 3. 推論鏈（理由）

1. **畫質設定不是主因**：解析度、FSR、陰影、物件密度、雲都調過，每項只差 1～4 FPS（數據見 §4.4）。
2. **CPU 計算本身不是主因**：VTune 顯示主執行緒只有約 31% 時間在算，其餘在等（§4.3）。快取／TLB 相關的停頓推估每幀約 3 ms【推論】。
3. **主執行緒等的是送出畫面**：VTune 顯示主執行緒每幀在 nvoglv64.dll 等一次、約 22 ms；Nsight 確認是 `vkQueuePresentKHR`，平均 18.7 ms（§4.5）。
4. **不是緩衝區張數或送出模式的問題**：自製 Vulkan layer 把 swapchain 改成 3 張、改 IMMEDIATE，送出畫面仍卡 20～25 ms（§4.7）。
5. **把等待從主執行緒拿掉也沒用**：自製 layer v3～v6 逐步把 present、acquire、submit 都與 X-Plane 主執行緒解耦，FPS 始終約 20～21（§4.7）。
6. **v6（虛擬 swapchain）直接顯示輸出路徑的吞吐量**：負責輸出的 worker 執行緒每幀「向驅動 acquire 約 24 ms + present 約 24 ms ≈ 47 ms」，正好等於一幀。X-Plane 主執行緒反過來要等虛擬緩衝區空出來（18～22 ms/幀）。→ 輸出路徑每秒只能送約 21 張【推論，但有量測支持】。
7. **輸出路徑的速度與顯卡負載有關**：主選單時同一路徑每次只要 0.5～1 ms、FPS 110～150；飛行時約 24 ms。降低顯卡負擔（FSR 效能、關雲）FPS 會小幅上升（§4.4、§4.7）。→ 推論驅動把畫面複製到內顯的工作與 X-Plane 繪圖互相影響【推論】。
8. **Windows 11 的 CASO 可減少一次複製，但需要 Intel 11 代以後**，這台是 10 代，不適用（§4.9）。

---

## 4. 全部量測數據

### 4.1 早期觀察
- 【使用者描述】Afterburner：各 CPU 執行緒使用率約 50%，偶爾 80%。CPU 溫度約 95°C、時脈約 3.3 GHz（另一段時間 PresentMon 顯示 3.5～3.7 GHz）。
- 【使用者描述】VRAM：專用約 2.2/4.0 GB，共用約 1.7 GB。
- 【使用者描述】關掉外掛約差 6 FPS。
- 【使用者描述】X-Plane 內建 frame time 數據曾顯示 CPU 約 13 ms（數字未保存）。
- 【使用者描述】曾試過散熱相關措施，溫度壓不下來。

### 4.2 PresentMon #1（2026-10-29，264 幀，9.9 秒）
| 欄位 | 平均 |
|---|---|
| Runtime / Present mode | DXGI，SyncInterval 0，AllowsTearing 1，**Hardware Composed: Independent Flip** |
| MsBetweenPresents | 37.62 |
| MsCPUBusy / MsCPUWait | 37.11 / 0.51 |
| MsGPUBusy / MsGPUWait / MsGPUTime | 22.74 / 2.60 / 25.34 |
| GPUUtilization | 55.6% |
| GPU / CPU 頻率 | 1769 MHz / 3683 MHz |
| GPU 溫度 | 74°C |
| VRAM | 2.46 / 4.29 GB |

### 4.3 Intel VTune 2024.3
（VTune 2025 以後不支援 Comet Lake，改用 2024.3。皆為 Attach 到 X-Plane.exe 30 秒，地點相近但不完全相同。）

**Microarchitecture Exploration（沒有自製 layer）**
- Clockticks 105.33e9；Instructions retired 112.21e9；CPI 0.939；平均頻率 3.7 GHz；42 條執行緒。總 CPU 時間約 28.5 秒（≈ 平均 0.95 條邏輯執行緒）。
- Top-down（% of pipeline slots）：Retiring 34.0；Front-End Bound 21.2（Latency 16.7：ICache 5.8、**ITLB 6.3**、Branch resteers 7.2、DSB switches 2.5、LCP 0.1、MS switches 2.1；Bandwidth 4.5）；Bad Speculation 7.7；Back-End Bound 37.1（Memory 19.0、Core 18.1）。
- Core Bound：Divider 1.5；Port utilization 21.0（0 ports 25.5、1 port 8.9、2 ports 8.1、3+ ports 19.0）；Vector capacity usage 20.0。
- Memory Bound 細項（% of clockticks）：L1 Bound 6.6（DTLB Overhead 100%、Load STLB Hit 100% ← 判斷為多工取樣造成的異常值；**Load STLB Miss 5.7**；Store forwarding blocked 0.4；Lock 0；Split loads 0；4K aliasing 0.4；**FB Full 30.1**）；L2 Bound 0.0；L3 Bound 5.4（Contested 1.1、Data sharing 0.3、L3 latency 4.8、SQ full 0.2）；**DRAM Bound 23.2**（Bandwidth 17.6、Latency 24.7）；Store Bound 0.7。
- 依模組的 CPU 時間（秒，CPI）：x-plane.exe 11.410（0.959）；**fmod.dll 5.938（0.638）**；ntoskrnl.exe 3.558（1.533）；ntdll.dll 2.286（0.793）；nvoglv64.dll 1.893（1.437）；fmodstudio.dll 0.880（2.406）；nextatc.xpl 0.533（0.646）；openal32.dll 0.331（0.697）；nvwgf2umx.dll 0.161（5.245）；kernelbase.dll 0.158；xplm_64.dll 0.136；kernel32.dll 0.133；win.xpl 0.126。（fmod 偏高可能與持續播放的煞車音效有關。）
- 依執行緒的 CPU 時間：TID 30788（主執行緒）11.100 s（CPI 0.983）；22040 5.033 s（CPI 0.581，推測為 fmod）；15828 2.080；11768 1.966；30076 1.376；7092 1.145；20776 1.126；31128 1.113；24716 1.107；18192 0.562；26324 0.415（CPI 4.026）。
- 熱點函式（無符號）：func@0x1406c4c83 1.524 s（CPI 1.544）；func@0x1407a6ce0 0.289 s（CPI 21.3）。最熱的函式也只占總 CPU 時間約 5%。

**Threading（沒有自製 layer）**
- 主執行緒 TID 30788：Effective 9.171 s、**Spin 5.207 s、Wait 14.942 s**（30 秒內），Wait count 1,662。
  - nvoglv64.dll func@0x180f965f0：Effective 0.058 s、Spin 3.360 s、**Wait 13.597 s、765 次**（≈ 每幀一次，平均約 22 ms/次）
  - nvoglv64.dll func@0x180e0ac1f：Effective 0.043 s、Spin 1.463 s、Wait 1.322 s、604 次
  - nvoglv64.dll func@0x180e47620：Effective 0.429 s、Spin 0.039 s
- 工作執行緒（入口 func@0x140c513a0）：TID 15828 Effective 11.650 s、TID 8032 8.731 s；另外四條（24716、31128、20776、7092）各約 0.1 s Effective、1.4 s Spin、28.4 s Wait、約 26,600 次 Wait。
- Sync object 總等待（所有執行緒加總）：X-Plane 建立的物件 0x140c8ea40 113.56 s（105,326 次）；OpenAL32 物件 89.56 s；Sleep 71.38 s；X-Plane 物件 0x140c8ef70 67.16 s；nvwgf2umx 物件約 60 s、51 s；fmod 物件 59.60 s；多個 nvoglv64 物件各約 30 s。

**Threading（自製 layer v4 啟用時；場景不完全相同，工作執行緒可能在載入地景）**
- 主執行緒 TID 8076：**Effective 17.892 s、Spin 4.532 s、Wait 6.164 s**。
- 主執行緒 CPU 時間最多的是 nvoglv64.dll func@0x180e0ac1f：15.4%（3.455 s / 22.424 s），由 X-Plane func@0x140622690 呼叫。
- 工作執行緒 29716 / 28216 / 22580 Effective 9.718 / 6.379 / 5.696 s（比無 layer 時多很多，原因未確認）。
- ⚠ 未解釋：主執行緒 Effective 時間比無 layer 時多約一倍，但 FPS 沒變。

### 4.4 畫質設定測試
| 測試 | 結果 |
|---|---|
| FSR 原本 → 效能（肉眼看 FPS）【使用者描述】 | 21～24 → 23～26 FPS |
| PresentMon，FSR 效能（23:09，257 幀） | 25.67 FPS；MsBetweenPresents 38.95；CPUBusy 38.39；CPUWait 0.56；GPULatency 23.39；GPUTime 25.08；**GPUBusy 23.07**；GPUWait 2.00；UntilDisplayed 12.04；GPU 使用率 53.0%；CPU 使用率 51.8%；CPU 頻率 3466 MHz |
| PresentMon，FSR 原本（23:10，232 幀） | 23.29 FPS；42.93；42.36；0.57；25.00；27.44；**25.18**；2.26；11.89；52.7%；38.7%；3701 MHz |
| 陰影品質、物件密度各降一級【使用者描述】 | 「都沒差多少」（無數字） |
| 天氣改晴天無雲【使用者描述】 | 約 23～26 → 25～27 FPS |

（兩次 PresentMon 都是 Independent Flip。FSR 效能只算 25% 像素，GPUBusy 只少 2.1 ms，每幀少 3.9 ms。）

### 4.5 NVIDIA Nsight Systems 2026.5.1（由 nsys 啟動 X-Plane，飛行中錄約 20 秒）
Vulkan API Summary：
| 函式 | 次數 | 總時間 | 平均 | 中位數 | 最小 | 最大 |
|---|---|---|---|---|---|---|
| vkQueuePresentKHR | 372 | 6.943 s（78.7%） | 18.665 ms | 19.570 ms | 9.919 ms | 27.293 ms |
| vkQueueSubmit | 6,775 | 471 ms | 69.5 µs | 58.0 µs | 11.7 µs | 1.119 ms |
| vkCreateSwapchainKHR | 1 | 347 ms | | | | |
| vkCmdBindPipeline | 309,803 | 298 ms | 961 ns | | | |
| vkAllocateMemory | 36 | 280 ms | 7.784 ms | | | 247.5 ms |
| vkUpdateDescriptorSetWithTemplate | 373,849 | 121 ms | | | | |
| vkAllocateDescriptorSets | 11,750 | 78 ms | | | | |
| vkAcquireNextImageKHR | 372 | 9.6 ms | | | | |

- 錄製期間約 18.5 FPS（nsys 本身有額外負擔）。
- 我用 SQLite 匯出檔自行計算（VULKAN_WORKLOAD 表＝每次 submit 在 GPU 上的實際執行區間，共 6,773 筆；contextId 2×5866、4×744、3×163）：
  - 整段時間 GPU 執行 X-Plane 工作的比例：**43.2%**，平均每幀 **23.38 ms**
  - 每次 present 呼叫期間，GPU 在跑 X-Plane 工作的比例：平均 **52.0%**（中位數 50.8%）
  - GPU 做完 X-Plane 最後一批工作後，present 還要再過 **平均 8.52 ms（中位數 8.07）** 才返回
  - present 返回後到下一批 X-Plane GPU 工作開始：平均 5.00 ms（中位數 1.09）
  - 典型一幀（相對時間 ms）：46.37 GPU 開始主要繪圖（16.0 ms）→ 56.67 CPU 呼叫 present → 62.40～68.13 GPU 最後一段 → 79.96 present 返回 → 約 90.5 CPU 才送出下一批主要工作（中間 CPU 只送了零星小工作）
- ETW（DXGI／DxgKrnl）事件：
  - DXGI Present 事件 SyncInterval 0
  - DxgKrnl Present 事件 Flags = **"RedirectedFlip"**（⚠ 與 PresentMon 的 "Independent Flip" 看似不一致，原因未確認）
  - 有 JournalEntry「Failed to find an output for the swapchain」
  - 有 SwapChain 事件 BackbufferCount 2（不確定是否屬於 X-Plane）
  - 記錄到的 GPU：NVIDIA GTX 1650、Intel UHD Graphics、Microsoft Basic Render Driver
  - SwapChain 事件含 HybridPresentMode、CrossAdapterTierSupport、CASOFallbackReason 欄位，**尚未讀取其值**
- 早期兩次 nsys 錄製只錄到載入階段（present 只有 6～8 次），已排除。

### 4.6 Vulkan SDK API dump（X-Plane 的 swapchain 參數）
- `vkCreateSwapchainKHR`：**minImageCount 2**；VK_FORMAT_B8G8R8A8_SRGB；1920×1080；usage TRANSFER_SRC | TRANSFER_DST | COLOR_ATTACHMENT；EXCLUSIVE；IDENTITY；OPAQUE；**presentMode MAILBOX**；clipped 1；pNext 帶 VkSurfaceFullScreenExclusiveInfoEXT = **DISALLOWED**
- `vkGetSwapchainImagesKHR`：驅動實際建立 **2** 張
- X-Plane Log：可用 present mode 有 MAILBOX、IMMEDIATE、FIFO、FIFO_RELAXED、FIFO_LATEST_READY；「Disabling fullscreen exclusive mode for swapchain」

### 4.7 自製 Vulkan implicit layer 測試
（只在 X-Plane.exe 內作用；log 每 300 幀輸出一次。FPS 由 present 時間間隔計算。飛行地點相同或相近。）

| 版本／設定 | 做法 | FPS | 送出畫面阻塞時間 | 主執行緒其他等待 |
|---|---|---|---|---|
| v1，image_count=0（原本 2 張） | 只量測 | 約 20 | 19.3～20.3 ms（主執行緒） | — |
| v1，image_count=3（驅動確實建 3 張） | 增加 swapchain 張數 | 約 20 | 20.4～21.8 ms | — |
| v1，present_mode=IMMEDIATE | 改送出模式 | 約 19.4 | 21.4～25.6 ms | — |
| v2，async 全部 queue 操作 | submit 與 present 都丟到 worker | — | — | **GPU 崩潰**：VK_ERROR_DEVICE_LOST @ X-Plane gfx_vk_event.cpp:46，約 90 秒後，還沒到 300 幀 |
| v3，只把 present 丟到 worker（同一 queue） | | 19.4～20.9 | 20.3～21.5 ms（worker） | 主執行緒 submit 時等 present 完成：9.8～14.2 ms/幀；acquire 0 |
| v3 在主選單 | | 111～154 | **0.47～1.06 ms**（worker） | acquire 0.19～0.80 ms |
| v4，present 改用額外的 queue（family 0 index 1） | | 20.7～21.2 | 22.2～23.3 ms（worker） | submit 0、acquire（layer 端）0～0.02 |
| v5 A：async=0（原本行為 + 各函式計時） | | 19.2～20.5 | 21.3～23.9 ms（主執行緒） | **vkAcquireNextImage 1.15～1.97 ms/幀**；vkQueueSubmit 0.61～0.74 ms（17～18 次/幀）；vkGetEventStatus ≈0（1 次）；vkGetFenceStatus ≈0.01（17～19 次）；vkGetQueryPoolResults ≈0（8 次） |
| v5 B：v4 模式 + 計時 | | 19.9～20.6 | 23.4～24.3 ms（worker） | **vkAcquireNextImage 21.2～24.0 ms/幀**（驅動內部，不是 layer 造成）；submit 0.66～0.69 |
| v6：虛擬 swapchain（X-Plane 畫進 layer 自建的 3 張影像；worker 向驅動 acquire 真的影像、在額外 queue 上 vkCmdCopyImage、再 present） | | **20.4～21.4** | worker present 23.2～24.1 ms；**worker 真 acquire 23.1～24.1 ms/幀** | X-Plane 的 present 呼叫只要 0.004～0.005 ms；**X-Plane 等虛擬影像空出來 18.5～22.4 ms/幀** |

### 4.8 NVIDIA「Vulkan/OpenGL 展現方法」
- 測試期間 X-Plane 的設定一直是「喜好原始設定」（= Prefer native）。使用者早期誤以為「原始設定」是指原本的設定。
- 「喜好在 DXGI Swapchain 上分層」**沒有用數字量測過**（只有早期肉眼比較，結論是原生較好）。
- ⚠ 即使設定為 Prefer native，PresentMon 仍顯示 Runtime 為 DXGI。

### 4.9 外部資料
- Microsoft DirectX Blog（2023-09）：無 MUX 的混合筆電，畫面要複製兩次才能顯示；**CASO** 可減為一次（平均 FPS +16%、延遲 −27%）。需求：Intel 11 代 Iris Xe 以後或 AMD Ryzen 6000 以後、WDDM 3.0+、Windows 11 22000.1817+/22621.1413+；NVIDIA 驅動 r515+。文中只提 DX11/DX12。
- NVIDIA 開發者論壇：有桌機 RTX 4090 使用者回報 `vkQueuePresentKHR` 在 nvoglv64 內切換到 DX12 context 做複製並阻塞 GPU 工作；改成「Prefer native」後 present 從 0.4 ms 降到 0.02 ms（與本機情況不同：本機已是 Prefer native）。

---

## 5. 與結論不完全吻合、或還沒解釋的地方

1. **GPU 並沒有滿載**：PresentMon 的 GPU 使用率約 53～56%；Nsight 顯示 GPU 跑 X-Plane 工作只占 43%。如果「複製工作跟繪圖搶顯卡」是吞吐量問題，GPU 應該接近滿載才對。→ 瓶頸可能是**同步延遲**（獨顯完成 → PCIe 傳輸 → 內顯合成／DWM → 釋放影像），而不是 GPU 的運算量。目前無法區分。
2. **Present 模式描述不一致**：PresentMon 說 Independent Flip，DxgKrnl ETW 說 RedirectedFlip。
3. **v4 底下主執行緒 Effective CPU 時間加倍**（9.2 → 17.9 秒）而 FPS 不變，原因未知；那次工作執行緒也明顯較忙，場景可能不同。
4. **v2 崩潰的真正原因**未確認（推測是 OpenGL bridge 與延後 submit 的同步衝突）。
5. 各測試的飛行地點、時間點不完全相同，FPS 有 ±1 的波動。
6. CPU 溫度約 95°C，可能有降頻；沒有在同一次測試中記錄頻率與溫度。
7. 「複製到內顯的工作與 X-Plane 繪圖互相影響」是推論，沒有直接觀察到驅動內部或內顯端的工作。

---

## 6. 尚未測試的項目
- 硬體加速 GPU 排程（HAGS）開／關
- 內建螢幕更新率改 60 Hz（目前更新率未確認）
- 「喜好在 DXGI Swapchain 上分層」的數字量測
- 停用其他隱式 Vulkan layer（Overwolf overlay、OBS hook、Steam overlay、VK_LAYER_NV_present 等）
- 讀取 ETW SwapChain 事件的 HybridPresentMode／CrossAdapterTierSupport／CASOFallbackReason
- 外接螢幕（HDMI 是否直連獨顯也未確認）
- 不同版本的 NVIDIA／Intel 內顯驅動
- X-Plane 視窗模式與全螢幕（X-Plane 會停用 fullscreen exclusive）
- 用 PresentMon 或 GPUView 看內顯（Intel UHD）在同一時間的負載

---

## 7. 想請其他 AI 回答的問題
1. §3 的推論鏈有沒有邏輯漏洞？v6 的結果能不能支持「瓶頸在驅動輸出路徑」？
2. §5 第 1 點（GPU 沒滿載）是否推翻「與繪圖搶顯卡」的說法？還有什麼機制能解釋「輸出路徑每幀約 47 ms，且隨顯卡負載變化」？
3. 有沒有漏掉的使用者端解法（設定、驅動、Windows 功能、第三方工具）？
4. §6 裡哪些測試最值得先做？

---

## 8. 後續結果：v7 繞過 NVIDIA 的 Optimus 輸出路徑（2026-11-01）

**做法**：X-Plane 畫進 layer 自己的影像；worker 執行緒用 GeForce 把每一幀複製到釘住的系統記憶體（兩張顯卡都用 `VK_EXT_external_memory_host` 匯入同一塊記憶體）；layer 在 Intel UHD 上另開一個 Vulkan 裝置，把畫面複製進 Intel 自己在 X-Plane 視窗上的 swapchain 再送出。NVIDIA 的 `vkQueuePresentKHR`／`vkAcquireNextImageKHR` 完全不再被呼叫。

**確認可行**：`vulkaninfo` 顯示 Intel UHD（driver 101.1999，Vulkan 1.3.212）與 GTX 1650 都支援 `VK_EXT_external_memory_host`。

**結果（機型 A330；這架比之前測試用的 Baron 58 更吃效能，前一天同機型不到 19 FPS）**：

| 項目 | 數值 |
|---|---|
| FPS | **31.7～34.1** |
| Intel 端 acquire | 0.00～0.01 ms/幀 |
| Intel 端 present | 0.02 ms/幀 |
| X-Plane 等空的虛擬影像 | 0 ms/幀 |
| GeForce 畫完＋複製到主記憶體的等待 | 19～29 ms/幀（主要是在等 GPU 畫完） |
| X-Plane 主執行緒每幀呼叫 vkGetFenceStatus | 1,100～2,500 次（主執行緒跑在前面、在等 GPU） |
| Intel swapchain | 1920×1080，3 張，IMMEDIATE（Intel 不提供 MAILBOX） |

**結論更新**：
- §5 第 1 點的疑問（GPU 沒滿載卻卡住）得到解答：瓶頸是 NVIDIA 驅動在 Optimus 上的輸出路徑實作，而不是 PCIe、內顯或 DWM 的硬體限制。同樣是「複製兩次」，自己做的版本每幀只要 1～2 ms。
- 繞過之後，瓶頸變成 GTX 1650 本身的繪圖速度，畫質設定重新會影響 FPS。
- 尚未完成：同一地點、同一機型的 off/on A/B 對照；長時間穩定性；其他筆電的重現。
