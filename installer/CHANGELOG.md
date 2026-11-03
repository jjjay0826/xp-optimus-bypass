# Installer 1.5 / DLL logging update — 2026-11-03

- DLL directly selects the installed root log, or XP_OPTIMUS_BYPASS_LOG when set.
- Shareable, unbuffered per-message writes with explicit flush; no hard links required.
- Live viewer refreshes native and launcher diagnostics once per second.
- No placeholder log in the download ZIP. Missing registration/output is reported separately.
- FPS aggregation remains 300 frames for comparability; no claim of one-second FPS samples.
- Actions builds installer-r1 from an explicit source ref, checks Windows syntax/languages,
  and appends its own bounded checksum notes without replacing the original ZIP.
- DLL source version remains 0.9.1.1-beta, but rebuilt DLL hashes change.

- DLL 直接寫正式安裝位置第一層 log，亦支援明確指定路徑。
- 每筆紀錄即時刷新，允許檢視視窗同時讀取，不再依賴硬連結。
- 新增每秒更新的 log 檢視與分開的啟動診斷，下載包不再附空白 log。
- FPS 統計保留每 300 幀；不是每秒一筆 FPS。
- Actions 可指定來源分支建置並檢查 Windows 語法／語系，保留原 ZIP。
- DLL 版本標示維持 0.9.1.1-beta，但本次重新編譯後校驗碼不同。

Validation: static checks and reproducible packaging can run locally. Windows/X-Plane
integration, actual DLL loading, file sharing and live UI behavior still require runtime tests.
驗證範圍：靜態及可重現封裝可在本機檢查；Windows/X-Plane 載入、共享讀取與 UI 仍須實測。
