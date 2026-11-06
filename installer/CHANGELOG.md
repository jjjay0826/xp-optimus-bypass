# 0.9.3.2 / Installer — 2026-11-06 (DLL 0.9.3.2)

- Anonymous reports carry a random installation ID (`install_id`, kept in `app\report_id.txt`, kept on reinstall,
  removed on uninstall). It groups the reports of one installation; "Reset anonymous ID" in the report window makes a
  new one. The install notice and the README say so.
- 匿名回報加上隨機的安裝 ID（`app\report_id.txt`，重裝保留、解除安裝刪除），只用來把同一份安裝的回報歸在一起；
  回報視窗可「重設匿名 ID」。安裝提醒和 README 已更新說明。

# 0.9.3 / Installer — 2026-11-04 (DLL 0.9.3)

- Global mode moved under an advanced checkbox and confirmed with an anti-cheat warning.
- Detects a ZIP that was not extracted and restricted (Constrained Language) PowerShell; removes the download mark
  from its own files; `installer-error.log` records PowerShell / Windows version and the script stack.
- New `fallback` folder: install / uninstall / start X-Plane without PowerShell.
- Anonymous compatibility reports: checkbox on by default, "See what is sent" preview, can be switched off at any time
  without reinstalling; the launcher sends one report after X-Plane closes. The log window has a Report button to
  copy the same report or open a GitHub issue.
- 全域模式移到「進階」並需確認反作弊警告。
- 偵測未解壓縮直接執行、PowerShell 受限；錯誤紀錄更完整；新增不需 PowerShell 的 fallback 資料夾。
- 匿名相容性回報：預設開啟、可預覽內容、隨時關閉；X-Plane 關閉後由啟動器送出一次。紀錄視窗新增「產生回報」。

## Configurable install location / 可選擇安裝位置

- Editable destination and folder picker, in both languages.
- Remember the installed path through the per-user uninstall registration.
- Derive application, docs, logs, manifest and shortcut paths from that destination.
- Accept new/empty folders; refuse unrelated nonempty folders and installer-source overlap.
- Changing an existing install location requires uninstalling first; retain installer sources for reinstall.
- 安裝位置可輸入與瀏覽；依使用者登錄記住位置，程式、log、文件、捷徑皆跟隨。
- 接受新建或空資料夾，拒絕其他非空資料夾及與安裝來源重疊的位置。
- 已安裝時更換位置須先移除再重裝，獨立安裝來源會保留。

# 0.9.2 / Installer and DLL logging update — 2026-11-03

- DLL directly selects the installed root log, or XP_OPTIMUS_BYPASS_LOG when set.
- Shareable, unbuffered per-message writes with explicit flush; no hard links required.
- Live viewer refreshes native and launcher diagnostics once per second.
- No placeholder log in the download ZIP. Missing registration/output is reported separately.
- FPS aggregation remains 300 frames for comparability; no claim of one-second FPS samples.
- Actions builds installer-r1 from an explicit source ref, checks Windows syntax/languages,
  and appends its own bounded checksum notes without replacing the original ZIP.
- DLL version is 0.9.2; the new release uses tag v0.9.2.

- DLL 直接寫正式安裝位置第一層 log，亦支援明確指定路徑。
- 每筆紀錄即時刷新，允許檢視視窗同時讀取，不再依賴硬連結。
- 新增每秒更新的 log 檢視與分開的啟動診斷，下載包不再附空白 log。
- FPS 統計保留每 300 幀；不是每秒一筆 FPS。
- Actions 可指定來源分支建置並檢查 Windows 語法／語系，保留原 ZIP。
- DLL 版本為 0.9.2，使用新 Tag v0.9.2，校驗碼會重新產生。

Validation: static checks and reproducible packaging can run locally. Windows/X-Plane
integration, actual DLL loading, file sharing and live UI behavior still require runtime tests.
驗證範圍：靜態及可重現封裝可在本機檢查；Windows/X-Plane 載入、共享讀取與 UI 仍須實測。
