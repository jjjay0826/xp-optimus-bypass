# Runtime acceptance / 實機驗收

These checks are pending until run on Windows with X-Plane. / 以下須在 Windows 實際驗收。

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
