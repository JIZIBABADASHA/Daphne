# N26 待辦

尚未完成的工作與已知、暫不處理的問題。完成的項目移到對應的文件（N25 等），並從這裡刪除。

## 跨平台收尾（N25）

- **CI：用系統 FFmpeg 建置**。目前 CI 不裝 FFmpeg，`decoder_ffmpeg.cpp` 在 Windows 以外從未由 CI 編譯（`sscanf_s` 就是在 Mac 實機上才發現）。Ubuntu 加一個 job：`apt install libavformat-dev libavcodec-dev libavutil-dev libswresample-dev`。
- **CI：GCC / Clang 的警告**。CMake 只對 MSVC 開警告（`/W4`）；GCC / Clang 沒有加 `-Wall -Wextra`，CI 的「沒有警告」在那兩個平台沒有意義。目前手動確認過 GCC 15.3 `-Wall -Wextra` 為 0 個警告。
- **`cross-platform-plan` 合併回 `main`**，之後讓 MIKU 的 `Extensions/Daphne` 指向新的版本（MIKU 端另發 PR）。

## 尚未驗證

- **MSVC ARM64 建置**（Windows on ARM）：程式已避開 x86 專屬的 cpuid、FPCR 改用 `_ReadStatusReg` / `_WriteStatusReg`，但沒有 ARM64 工具組，尚未實際編譯。
- **Linux 即時排程**：處理執行緒拿到 `SCHED_FIFO` 時，由它建立的執行緒（濾波器設計、`parallel_for`、無縫換軌預備）會先回到 `SCHED_OTHER`。需要有即時權限（`RLIMIT_RTPRIO` / `CAP_SYS_NICE`）的 Linux 實際確認。
- **macOS universal 建置的 arm64 部分**：在 Intel Mac 上建置成功（x86_64 有 AVX2 核心、arm64 沒有），但 arm64 只由 CI（macOS 14 原生）執行過，沒有執行過 universal 二進位檔的 arm64 部分。
- **較舊的 FFmpeg**：pkg-config 建置只用 FFmpeg 8.0、8.1 驗證過；Debian 12 套件庫的 FFmpeg 5.1 等舊版本未測試。
- **Windows 上的小項目**：
  - 本分支與 `main` 的效能比較（`parallel_for` 的 worker 優先權在 Windows 改成三級，原本照抄建立者的確切值），用最重的設定比較 underrun 數與設計時間。
  - `decode-test --md5`（內建 MD5，取代 BCrypt）與 `main` 的結果逐檔比對。
  - 含中文、emoji、超過 260 字元的實際檔名播放（孤立 surrogate 已有單元測試）。
  - UTF-8 版 C API（`dc_load_utf8` 等）目前沒有測試。

## 拆分計畫的後續步驟（N25）

| 步驟 | 內容 |
|---|---|
| 5 | 即時輸入的 `LiveBus`：共用記憶體（Windows MIKU）與 Unix socket（macOS MIKU 的 `live-relay.js`），含安全卸離 |
| 6 | Linux ALSA 輸出 |
| 7 | macOS Core Audio 輸出（參考 MIKU 的 `mac/native/audio.mm`：hog mode、實體格式、取樣率確認） |

- 加入 Core Audio / ALSA 時，`player` 依輸出 API 判斷的三件事（有沒有原生 DSD、是不是共享混音、緩衝長度由誰設定）改成由裝置輸出提供能力，播放器不再認得 API 名稱。
- 步驟 7 時一併重新檢查 macOS 的 `RealtimeScope`：目前是 time-constraint policy（每 10 ms 最多 5 ms），但處理執行緒是一次補滿約 0.5 s 環的吞吐型工作，超過宣告的計算時間可能被系統降級。

## 已知問題（暫不處理）

- **即時輸入（YouTube Music）延遲大**：兩層緩衝（MIKU 環 0.3 s + 播放器環 0.5 s）。
- **不同取樣率的無縫換軌與交叉淡化的預熱在處理執行緒上同步執行**：長濾波器時可能超過環的 0.5 s。
- **處理執行緒的 sleep 輪詢**（環滿時 2 ms、排空時 5 ms、等待裝置的迴圈）：之後改成事件等待。

## MIKU 擴充（`src/Daphne.Extension`）

- **裝置失效（0x88890004，AUDCLNT_E_DEVICE_INVALIDATED）**：設定中的裝置不在（例如 DAC 關機）時，MIKU 自己的核心會退回預設裝置，Daphne 只回報錯誤。改成退回預設裝置，或顯示看得懂的訊息。
- **播放路徑卡片**：
  - 加上 ReplayGain 的狀態（目前是否套用、增益多少）。
  - DSD 直送時，DSP 區加一列說明（直送不經任何處理）。
- **設定頁**：不升頻時也顯示「濾波器暫存」那一列（目前只在升頻或轉 DSD 時出現）。
