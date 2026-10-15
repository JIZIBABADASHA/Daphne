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

### 7. macOS Core Audio 輸出與 MIKU 的 Mac 外殼

拆成四個 PR：7a 輸出本身、7b 排程、7c helper 程序、7d MIKU 的 Mac 外殼。

**7a. Core Audio 輸出（分支 `coreaudio-output`，PR #20）✅**

- [x] Core Audio 輸出（共享 / 獨佔），照 MIKU 的 `mac/native/audio.mm`（N24「Core Audio」）：
  - 設定 nominal rate 後輪詢確認，不接受就以裝置實際的取樣率繼續（播放器重新取樣）並留下記錄；
  - 獨佔時才把 DAC 實體格式調到該取樣率下位元數最多的整數格式（至少 24 bit），已足夠就不動（不重新鎖定）；
  - 格式變更後等 HAL 回報的格式連續兩次相同才開始；
  - hog 住的裝置只在運轉中改設定：`open()` 先啟動 IO（靜音）再改取樣率與格式，`close()` 在 IO 運轉中還原，最後才停止 IO、放掉 hog；
  - 裝置拔除（`failed()`）、外部改取樣率（`reset_requested()`）的通知。
- [x] `output.api` 加上 `coreaudio_exclusive` / `coreaudio_shared`，預設是平台的獨佔輸出（`kDefaultOutputApi`）；CLI 的 `--shared` / `--exclusive` 跟著平台。
- [x] DSD 只走 DoP（macOS 沒有原生 DSD 的 API）：裝置格式是 `int24-in-32`，DoP 照常寫入，回呼精確轉成 float32。（程式完成；實機驗證要外接 DAC，見下）
- [x] 裝置列舉：`list_coreaudio_devices()`、`daphne devices`、C API `dc_devices` 的 `coreaudio` 陣列。
- [x] 實機驗證（Intel Mac、macOS 11.7、內建輸出）：隱藏測試 `[.coreaudio]` 3 個、CLI 跨取樣率連播、升頻、兩個程序搶裝置（N24「驗證」）。
- [ ] 外接 DAC 驗證：bit-perfect 錄音比對（需要 loopback 裝置，例如 BlackHole）、DoP（DSD64 需要 176.4 kHz，內建輸出最高 96 kHz）、拔除 DAC、其他程式改取樣率。
- [ ] 換曲時保留裝置（之後）：現在每次重開裝置都先還原再設定取樣率（跨取樣率的下一首多一次 DAC 重新鎖定，約 0.5–1 s）；MIKU 的作法是保留裝置一段時間、同一台裝置直接改取樣率。

**7b. 排程（不改程式，量測結果記在實作筆記）✅**

- [x] 重新檢查 macOS 的 `RealtimeScope`：目前是 time-constraint policy（每 10 ms 最多 5 ms），但處理執行緒是一次補滿約 0.5 s 環的吞吐型工作，超過宣告的計算時間可能被系統降級。候選：處理執行緒改用 QoS user-interactive，真正的即時工作只留在 IOProc（環 → 裝置，已是 HAL 的即時執行緒）；`WorkerPool` 的 worker（步驟 0 起跟著拿 time-constraint）一起決定。在 Mac 上加 CPU 負載量 underrun 決定。→ 結論：維持 time-constraint（滿載下比 QoS user-interactive 多拿到約 35% 的處理速度，兩者都沒有 underrun）。

**7c. helper 程序（分支 `mac-helper`，PR #21）✅**

- [x] 一個執行檔，stdin / stdout 每行一個 JSON（與 MIKU 的 `miku-audio` 相同的模式），由 JS 外殼啟動（`helper/main.cpp`，`daphne-helper`，協定見 N4）。掛掉時重啟並接回原本的位置是 JS 外殼的事（7d）。
- [x] 讓出與取回裝置（A 方案必需）：Mac MIKU 播 YouTube Music、或 MIKU 不在前景且沒有播放時，會檢查 engine 的 `holdsDevice`，呼叫 `stop({ release: true })` 或 `releaseDevice()`；Daphne 要完全放開裝置（還原取樣率與格式、放掉 hog，7a 的 `close()` 已經做到），下一首曲庫的歌再取回。對照 MIKU 的 `native-audio.js`。→ helper 的 `stop` 就是讓出（`verify/helper.py` 在 Mac 上確認 stop 之後沒有裝置被 hog、取樣率還原），下一次 `load` 取回；`holdsDevice` 由 JS 外殼依載入 / 暫停狀態回答（7d）。

**7d. MIKU 的 Mac 外殼（分支 `mac-shell`）**

- [x] `miku/extension.js`（MIKU 從 `Extensions/<名稱>/miku/` 載入，提供 `core.createEngine`），外殼回報 `supportsLive = false`。不需要改 MIKU。移植自 Windows 擴充（`DaphneExtension.cs`、`DaphneEngine.cs`、`DaphneOptions.cs`、`DaphnePath.cs`），底下是 helper；頁面檔案共用（`miku/web` symlink）。helper 掛掉時重新啟動並從原位置繼續（一分鐘最多 3 次）。
- [x] 驗證：Intel Mac（macOS 11）上用模擬 MIKU 的腳本測 helper、讓出 / 取回裝置。（`verify/miku_shell.js`，獨佔與共享都全部通過）
- [ ] MIKU 介面內的整合測試：MIKU 的 Mac 版用 Electron 44，需要 macOS 12 以上，介面內的整合測試要等有新的 Mac（或試舊版 Electron 的開發模式）。

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

**步驟 7a（`coreaudio-output`）**

- DoP 與 float32：Daphne 的 DoP 只接受 24 / 32-bit 整數的裝置格式，而 Core Audio 的 IO 回呼收的是 32-bit 浮點。輸出對播放器宣稱 `int24-in-32`，回呼再除以 2^31 轉成浮點：24-bit 整數在 float32 裡精確，HAL 再轉成 DAC 的整數格式也精確，PCM 與 DoP 都不必另外處理。
- 「系統輸出」在 hog 之後會消失：第一次兩個程序搶裝置的測試裡，第二個程序拿到的是「no system output device」——裝置被 hog 時 macOS 把系統輸出移走，內建輸出是唯一的裝置，系統輸出就變成沒有。改成：本程序 hog 住的裝置仍算系統輸出（同一個程序重開時也找得到），系統輸出不存在而有裝置被別人 hog 時回報是誰佔著。
- Apple clang 12 的 `-Wunused-private-field`：`RealtimeScope::handle_` 只有 Windows 用，在這台 Mac 以 `DAPHNE_WARNINGS_AS_ERRORS` 建置時才報錯（CI 的 Xcode 15 不報，之前在 Mac 上沒有開 `-Werror`）；加上 `[[maybe_unused]]`。
- 隱藏測試（`[.coreaudio]`）只在實機上手動跑：CI 的 macOS runner 不保證有音訊裝置。測試訊號是 −60 dBFS，夜裡在內建喇叭上跑也幾乎聽不到。
- 跨取樣率的下一首比較慢（1.5 s 的 96 kHz 檔播了 2.4 s）：關閉時還原 44.1 kHz、開啟時再設 96 kHz，DAC 重新鎖定兩次。列為之後的改進（保留裝置）。

**步驟 7b（排程的量測）**

- Intel Mac（i5-4308U，2 核 4 執行緒）、Core Audio 獨佔、48 k → 96 k、1048576 taps、double-double（無負載時 2.5 倍即時），另外開 4 個 `yes` 把 CPU 吃滿，各跑兩次：time-constraint（現在的作法）2.55 / 2.74 倍即時，QoS user-interactive 1.85 / 2.02 倍；underrun 都是 0，最長回呼間隔都在 16–26 ms。time-constraint 沒有因為超過宣告的計算時間而變慢，反而在滿載下拿到更多 CPU，所以維持不變（`WorkerPool` 的 worker 一樣）。實驗用環境變數切換，只在 Mac 上編譯，沒有進 repo。
- 順帶看到：1048576 taps 時 CLI 結尾的「last position」是 0 s（262144 taps 時 3.8 s），應該是濾波器延遲比整首還長時位置被截成 0，與 Core Audio 無關，待查。

**步驟 7c（`mac-helper`）**

- helper 底下直接用 C API（與 Windows 擴充相同），沒有另寫一套播放邏輯；JSON 用 nlohmann（已在 `third_party`）。
- `quit` 一開始在工作執行緒裡 `fclose(stdin)` 想打斷讀取執行緒的 `getline`，Windows 上打斷不了阻塞中的 pipe 讀取（`verify/helper.py` 的 quit 項目逾時）；改成由讀取執行緒自己處理 `quit`。
- 主程式結束時 stdin 讀到結尾，走與 `quit` 相同的清理：MIKU 當掉也不會把 DAC 留在 hog mode。

**步驟 7d（`mac-shell`）**

- 外殼介面以 MIKU 的 `AudioEngine`（`mac/app/main/audio.js`）為準：它把模組的 engine 當成 backend，用到 `load` / `pause` / `resume` / `seek` / `stop` / `release`、`holdsDevice`、`isPlaying` / `isLoaded` / `position` / `track` / `signal` / `st`、`applyVolume` / `applyDsp` / `applyReplayGain` / `applyOutput`、`listDevices` / `probeDevice`、事件 `changed` / `loading` / `failed` / `trackStarted` / `ended`。Mac 設定頁看核心的 `info` 旗標：`autoSampleRate: false`、`dsdModes`、`maxDsd`、`dsdPcmRate: false`；套用 Bit-perfect 時呼叫 `ext.daphne.bitPerfect`（關掉 Daphne 的升頻、apodize、double-double）。
- Mac 與 Windows 設定的差異：輸出模式是 `coreaudio` / `coreaudio-exclusive`；沒有 DAC 硬體音量（`hardware` 當成數位音量，與 MIKU 自己的核心相同）；DSD 只有 DoP（獨佔）或 PCM；EQ 多了 `LPQ` / `HPQ` 兩個類型名稱。
- `verify/miku_shell.js` 第一次跑有 5 項失敗，前 4 項是測試本身：替身的 `peekNext` 一直回傳 B（B 播完又排 B，無限接續），1 秒的檔案在設定變更前就播完了。改成 B 之後沒有下一首、後半段用 8 秒的檔案。
- 第 5 項是真的問題（2x 升頻打開後裝置留在 44.1 kHz，約 5 次 2 次）：播放器重啟裝置時先關舊的、再開新的；舊的放掉 hog 之後 macOS 還沒把系統輸出移回來，新的找不到「系統輸出」（「no system output device」），播放器的 88.2 kHz 嘗試失敗，退回清單裡的 44.1 kHz，而且失敗的嘗試訊息被丟掉。修正見 N24「系統輸出」（記住剛放掉的那一台 3 秒、最多等 1 秒）。播放器把非最後一次的嘗試失敗也記錄下來會比較好查，列為之後的改進。
- 同一個空窗期也讓 `probeDevice` 在 stop 之後一瞬間找不到系統輸出：改成退回剛才播放的那一台，再退回清單第一台。
- stop 之後 0.5 秒查詢時偶爾看到 88.2 kHz（最後的 `bitPerfect` 重啟與 stop 幾乎同時）；2 秒後三次都回到 44.1 kHz，裝置最後都還原。
- 頁面檔案：MIKU 的載入器只讀模組資料夾下的 `web/`，而且只允許那個資料夾裡的檔案；`miku/web` 是 git 的 symlink（mode 120000），macOS / Linux checkout 時是真的 symlink，Windows（`core.symlinks=false`）是一個內容為路徑的文字檔，Windows 擴充不用它。MIKU 的 Mac 安裝包要把它展開成實際的檔案（之後打包時處理）。
- Mac 上用免安裝的 Node 20（`~/daphne-test/tools`）跑測試；MIKU 本身（Electron 44）需要 macOS 12，介面內的整合測試仍待新的 Mac。

