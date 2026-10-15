# N26 待辦

尚未完成的工作與已知、暫不處理的問題。完成的項目移到對應的文件（N25 等），並從這裡刪除。

## 跨平台收尾（N25）

- **`cross-platform-plan` 合併回 `main`**：等下面的拆分步驟 5–7 都完成後一次合併（處理速度偵測、即時輸入延遲、預熱移出處理執行緒已完成，見 N4 / N2）；之後讓 MIKU 的 `Extensions/Daphne` 指向新的版本（MIKU 端另發 PR）。

## 尚未驗證

- **MSVC ARM64 建置**（Windows on ARM）：程式已避開 x86 專屬的 cpuid、FPCR 改用 `_ReadStatusReg` / `_WriteStatusReg`，但沒有 ARM64 工具組，尚未實際編譯。
- **Linux 即時排程**：處理執行緒拿到 `SCHED_FIFO` 時，由它建立的執行緒（濾波器設計、`parallel_for`、無縫換軌預備）會先回到 `SCHED_OTHER`。需要有即時權限（`RLIMIT_RTPRIO` / `CAP_SYS_NICE`）的 Linux 實際確認。
- **macOS universal 建置的 arm64 部分**：在 Intel Mac 上建置成功（x86_64 有 AVX2 核心、arm64 沒有），但 arm64 只由 CI（macOS 14 原生）執行過，沒有執行過 universal 二進位檔的 arm64 部分。
- **較舊的 FFmpeg**：pkg-config 建置驗證過 FFmpeg 6.1（Ubuntu 24.04，CI）、8.0、8.1；Debian 12 套件庫的 FFmpeg 5.1 未測試。
- **Windows 上的小項目**：
  - 本分支與 `main` 的效能比較（`parallel_for` 的 worker 優先權在 Windows 改成三級，原本照抄建立者的確切值），用最重的設定比較 underrun 數與設計時間。
  - `decode-test --md5`（內建 MD5，取代 BCrypt）與 `main` 的結果逐檔比對。
  - 含中文、emoji、超過 260 字元的實際檔名播放（孤立 surrogate 已有單元測試）。

## 拆分計畫的後續步驟（N25）

順序：0 → 5 → 7，以 Windows 與 macOS 為主；6（Linux）暫緩。每一步一個分支、一個 PR，合進 `cross-platform-plan`。完成的項目打勾，實作時的發現記在最後的「實作筆記」。

### 0. 輸出能力（分支 `output-caps`，PR #18）✅

- [x] `player` 依輸出 API 判斷的三件事（有沒有原生 DSD、是不是共享混音、緩衝長度由誰設定）改成由各平台的輸出後端提供（`OutputCaps`），開裝置前就查得到；刪掉 `common.h` 的 `api_has_native_dsd` / `api_is_shared` / `api_takes_buffer_ms`，播放器不再認得 API 名稱。
- [x] Windows 的 `WorkerPool` worker：替處理執行緒工作時用 `RealtimeHelperScope`（MMCSS "Audio"），不再只是 `THREAD_PRIORITY_ABOVE_NORMAL`。
- [x] 驗證：Windows 全部測試、`live.py`（行為不變）；三平台 CI。（PR #18：Windows 93 個測試、`live.py` 全過、CI 四個 job 通過）

### 5. 即時輸入的安全卸離（分支 `live-bus`，PR #19）✅

範圍在 2026-10-16 縮小：MIKU 1.4.5（`a15a6f6`）起，Mac 上的 YouTube Music 一律由網頁經系統輸出播放，播放核心只需要讓出裝置（見步驟 7）；把網頁的聲音擷取下來、經 `live-relay.js` 交給核心的路線預設關閉（設定檔要寫 `"ytThroughCore": true`，原因是在真實網頁上啟動不穩定）。所以先做 A（網頁自己播），B（經過 Daphne）只保留接口。

- [x] 安全卸離：解碼器只在 `produce()` 裡碰共用的環，期間持有租約（`g_readers`）；`set_live_bus` 先清掉指標、等租約歸零，再換上新的環。返回之後 host 就能釋放記憶體，播放中也可以（Windows MIKU 的共用記憶體也受益）。
- [x] 測試：一條執行緒不停讀取，另一條反覆卸離 / 直接替換環，並在返回後立刻把舊的環填成 NaN 再釋放；輸出不得出現 NaN 或其他值。
- [x] B 的接口（未實作）：`live_connect` / `live_disconnect`（`live_source.h`）與 C API 的 `dc_live_socket`，目前一律回傳失敗；`dc_live_bus` 先結束 socket 來源再換環。

**B 之後要補做時（Unix socket 來源）**：路徑在 MIKU 給模組的 `host.liveSocket`；資料是交錯的 float32 立體聲、沒有標頭、可能在任何位元組切開（一個 frame 會跨兩次 `recv`）；`live-relay.js` 會重啟（MIKU 0.5 s 後再開），要自動重連；讀得慢的連線會被丟區塊（積壓超過 1 MB）；取樣率由 MIKU 呼叫 engine 的 `setLiveRate(rate)` 告知（網頁要求 48 kHz，`tap` 訊息回報實際值）；Mac 外殼要回報 `supportsLive = true`，MIKU 才會走這條路。

### 7. macOS Core Audio 輸出與 MIKU 的 Mac 外殼（分支另開）

- [ ] Core Audio 輸出（共享 / 獨佔），照 MIKU 的 `mac/native/audio.mm`：
  - 設定 nominal rate 後輪詢確認，不接受就改成重新取樣並留下記錄；
  - 獨佔時才把 DAC 實體格式調到該取樣率下位元數最多的整數格式（至少 24 bit），已足夠就不動（不重新鎖定）；
  - 格式變更後等 HAL 回報的格式連續兩次相同才開始；
  - hog 住的裝置只在運轉中改取樣率（停止時改取樣率再放掉 hog，之後 `AudioDeviceStart` 會卡住約 7 秒）；結束時先還原取樣率與格式，最後放掉 hog；
  - 裝置拔除、預設輸出改變、外部改取樣率的通知。
- [ ] DSD 只走 DoP（macOS 沒有原生 DSD 的 API）。
- [ ] 重新檢查 macOS 的 `RealtimeScope`：目前是 time-constraint policy（每 10 ms 最多 5 ms），但處理執行緒是一次補滿約 0.5 s 環的吞吐型工作，超過宣告的計算時間可能被系統降級。候選：處理執行緒改用 QoS user-interactive，真正的即時工作只留在 IOProc（環 → 裝置）；在 Mac 上加 CPU 負載量 underrun 決定。
- [ ] 讓出與取回裝置（A 方案必需）：Mac MIKU 播 YouTube Music、或 MIKU 不在前景且沒有播放時，會檢查 engine 的 `holdsDevice`，呼叫 `stop({ release: true })` 或 `releaseDevice()`；Daphne 要完全放開裝置（還原取樣率與格式、放掉 hog），下一首曲庫的歌再取回。對照 MIKU 的 `native-audio.js`。外殼回報 `supportsLive = false`。
- [ ] 裝置列舉（C API）給 MIKU 的設定頁。
- [ ] helper 程序：一個執行檔，stdin / stdout 每行一個 JSON（與 MIKU 的 `miku-audio` 相同的模式），由 JS 外殼啟動，掛掉時重啟並接回原本的位置。
- [ ] MIKU 的 Mac 外殼 `miku/extension.js`（MIKU 從 `Extensions/<名稱>/miku/` 載入，提供 `core.createEngine`）。不需要改 MIKU。
- [ ] 驗證：Intel Mac（macOS 11）上用 CLI 與模擬 MIKU 的腳本測 helper、Core Audio、讓出 / 取回裝置。MIKU 的 Mac 版用 Electron 44，需要 macOS 12 以上，介面內的整合測試要等有新的 Mac（或試舊版 Electron 的開發模式）。

### 6. Linux ALSA 輸出（暫緩）

- [ ] ALSA 輸出：`hw:`（獨佔、bit-perfect、DoP）與 `default`（共享，經 PipeWire / Pulse）；`libasound` 在建置時可有可無。
- [ ] double-double 的 `WorkerPool` worker 優先權：處理執行緒拿到 `SCHED_FIFO` 時，worker 只是 `SCHED_OTHER`（「高於一般」的 nice −5 沒有權限時失敗），可能拖慢處理執行緒；改用 `RealtimeHelperScope`，並確認兩核心的機器不會被 FIFO 的 worker 佔滿。

### 實作筆記

（依完成順序記錄）

**步驟 0（`output-caps`）**

- 能力要在開裝置**之前**知道（`pcm_stream` / `dsd_stream` 依它排出要嘗試的取樣率，`core.cpp` 依它決定 DSD 檔轉 PCM 的取樣率），所以不放在 `AudioOutput` 物件上，而是與 `OutputOpener` 並列的 `OutputCapsQuery`（`PlayerDeps::output_caps`，以 `OutputChoice` 查詢）。`change_plan` 的 `Playing` 帶著目前輸出的能力。
- 預設值 `OutputCaps{}`（沒有原生 DSD、不是共享、吃 `buffer_ms`）就是 WASAPI 獨占，也是測試假裝置原本的行為，所以既有的測試在三個平台都不變；macOS / Linux 的 `platform_output_caps()` 暫時回傳預設值。新增測試：假裝置宣告 `native_dsd` 時 DSF 直接以 DSD 開啟、宣告 `shared_mix` 時 dsd 核心拒絕。
- 錯誤訊息裡仍寫著 ASIO / WASAPI（「output.dsd = native needs an ASIO driver with a DSD mode」等），在 Windows 上正確；加入 Core Audio 時改成不提 API 名稱的寫法。
- worker 優先權沒有在建立時決定：每個重新取樣器有自己的 `WorkerPool`，背景預熱（N4）時替普通執行緒工作，換上之後替處理執行緒工作。改成 `run()` 記下呼叫者是否在 `RealtimeScope` 內（新的 `platform::in_realtime_scope()`，thread_local），worker 只在這個狀態改變時進入 / 離開 `RealtimeHelperScope`。這是三個平台共用的程式：Linux 的 worker 因此也會跟著拿 `SCHED_FIFO`（沒有權限時維持原狀），兩核心機器的實測仍待步驟 6；macOS 的 worker 拿 time-constraint，與 `RealtimeScope` 一起在步驟 7 重新檢查。

## 已知問題（暫不處理）

- **處理執行緒的 sleep 輪詢**（環滿時 2 ms、排空時 5 ms、等待裝置的迴圈）：之後改成事件等待。

## MIKU 擴充（`src/Daphne.Extension`，暫緩）

- **裝置失效（0x88890004，AUDCLNT_E_DEVICE_INVALIDATED）**：設定中的裝置不在（例如 DAC 關機）時，MIKU 自己的核心會退回預設裝置，Daphne 只回報錯誤。改成退回預設裝置，或顯示看得懂的訊息。
- **播放路徑卡片**：
  - 加上 ReplayGain 的狀態（目前是否套用、增益多少）。
  - DSD 直送時，DSP 區加一列說明（直送不經任何處理）。
- **設定頁**：不升頻時也顯示「濾波器暫存」那一列（目前只在升頻或轉 DSD 時出現）。

**步驟 5（`live-bus`）**

- 租約只包住 `produce()`（真正碰環的地方），不包住 `read()` 等待來源的 2 ms sleep：卸離最多等一次內插的時間。讀取端 `fetch_add` 之後才讀指標、卸離端清掉指標之後才讀計數，兩邊都是循序一致（seq_cst），所以「讀取端拿到舊指標」與「卸離端看到計數為 0」不會同時成立。
- 換環時先清掉指標、等計數歸零，再設定新容量、最後放上新指標：讀取端不會拿到新指標配舊容量（原本 `g_cap` 與 `g_base` 分開寫，換環的一瞬間可能錯配）。
- 壓力測試要確認真的抓得到錯誤：暫時拿掉等待那一行後，第一版（寫入方每 1 ms 推進 2400 frame）6 次只失敗 1～2 次，因為讀取端很快讀完，大部分時間在 `read()` 的 sleep 裡（Windows 上一次約 15.6 ms），卸離很少撞上讀取。改成每 10 µs 推進 48 frame 的細流之後，讀取端幾乎一直在環裡，拿掉等待時 6 次全部失敗（其中兩次直接存取已釋放的記憶體而當掉）；恢復等待後 10 次全部通過。
- 測試的等待用 `steady_clock` 忙等，不用 `sleep_for(1 ms)`：Windows 沒有提高計時器解析度時那是 15.6 ms，第一版測試因此跑了 25 秒（現在約 1.6 秒）。

