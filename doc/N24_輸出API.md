# N24 輸出 API（WASAPI / ASIO）

`core/include/daphne/engine/audio_output.h`、`wasapi_output.h`、`asio_output.h`、`devices.h`，以及對應的 `core/src/*.cpp`

## 共同介面（`AudioOutput`）

- `open` 協商格式（不開始播放）。
- `start(ring, eof)` 開始從環讀取：eof 設定且環讀空時 `drained()` 變真，裝置之後收到靜音直到 `stop()`。
- `format()` 是裝置實際鎖定的格式，`matches_request()` 表示是否等於要求。
- `rearm()`：串流結束後又搜尋回來時解除 drained。
- `failed()` / `failure()`：裝置在播放中失效（拔除、被其他程式獨占）。
- `take_notes()`：驅動訊息、取樣率變更、遲到的回呼，交給主程式的記錄。
- `reset_requested()`：驅動要求重新開啟（播放器會重開並在原位置繼續）。
- 暫停 / DoP / 電平委派給 N23 的送料。
- `probe_before_design()`：有些驅動接受了串流不了的取樣率，播放器先以靜音短暫啟動一次再設計濾波器（ASIO 為 true）。
- `supported_rates()`：開啟中的驅動接受的 PCM 取樣率，給主程式的設定畫面（ASIO）。
- `buffer_fallback()`：裝置不接受 `output.buffer_ms`，以預設緩衝執行（WASAPI 獨占）。
- 播放器只透過這個介面使用裝置：裝置由 `PlayerDeps::open_output` 建立，Windows 預設為 `platform_output_opener()`（`core/src/output_win.cpp`：WASAPI / ASIO）；macOS / Linux 目前沒有裝置輸出（N25）。

統計（`OutputStats`，原子變數）：回呼數、underrun、WASAPI 事件逾時（兩個緩衝長度、至少 50 ms 沒有裝置事件）、最長回呼間隔、ASIO 遲到回呼（相鄰回呼間隔 > 1.5 個緩衝區週期）。

## WASAPI

| 模式 | 做法 |
|---|---|
| 獨占 | 事件驅動（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`）。格式搜尋順序：取樣率為要求值、同家族逐次減半，再試 192k / 96k / 48k / 44.1k；樣本格式為 int32、24-in-32、float32、packed24、int16。每種組合先以 `IsFormatSupported` 檢查 |
| 共享 | 系統混音器格式；取樣率不同時請 Windows 轉換（`AUTOCONVERTPCM`），以正確速度播放（共享模式本來就不是 bit-perfect） |

- 緩衝長度：`output.buffer_ms`，0 為預設（獨占：裝置週期、至少 25 ms，一個緩衝等於一個週期；共享：100 ms），否則 10–200 ms（1–9 視為 10）。
  - 下限 10 ms：Windows 獨占模式常見的預設週期；更短的週期少有裝置接受，對播放也沒有好處（播放器本身預先處理約 0.5 秒，裝置緩衝只影響暫停與搜尋的反應速度）。
  - 上限 200 ms：下面 VB Hi-Fi Cable 的情形 50 ms 已經解決，200 ms 留 4 倍餘量；再長的話，暫停與搜尋都要等一整個緩衝才生效。
  - 搜尋時等裝置淡出的上限隨緩衝調整（兩個緩衝加 50 ms，至少 300 ms）：裝置下一次回呼才淡出，固定 300 ms 在長緩衝下會在淡出前就清空環形緩衝。
  - 時鐘粗糙的裝置需要較長的緩衝：VB Hi-Fi Cable 以 15.6 ms 的系統計時器計時（事件間隔 31 / 31 / 16 ms 輪流），25 ms 緩衝偶爾來不及，裝置端補幾 ms 的零（約 4 次 1 次；10 ms 每次都有，50 ms 16 次都沒有）。播放器的 underrun 計數看不到這種情形（環形緩衝始終有資料）。裝置不接受指定的長度時退回預設，並記錄一則訊息。MIKU 的「緩衝大小」（40–200 ms）對應這個設定。
- 緩衝區未對齊（`AUDCLNT_E_BUFFER_SIZE_NOT_ALIGNED`）時，以裝置回報的對齊大小重新初始化。
- 開始前先送一個緩衝區的靜音（pre-roll）。
- 裝置失效（`AUDCLNT_E_DEVICE_INVALIDATED`）→ `failed()`。
- 鎖定的格式與要求不同時，呼叫端看得到（播放器會依此重試或報錯，絕不以錯誤的速度播放）。

## ASIO

- 開啟流程：init（桌面視窗）→ 輸出聲道限制在硬體數量（多出的由處理鏈降混）→ `kAsioSetIoFormat`（PCM 或 DSD）→ setSampleRate，之後最多讀回 3 次、每次間隔 5 ms 並重新設定 → 偏好緩衝大小（驅動沒給時用 512）→ createBuffers。
- 驅動以 CoCreateInstance 建立，失敗時直接載入 InprocServer32 的 DLL。
- 裝置取樣率是驅動的讀回值：硬體沒有鎖定要求的取樣率時，呼叫端看得到。
- 每一種 ASIO PCM 樣本型態都正確寫入：Int32LSB16..24 右對齊，MSB 型態交換位元組。
- 驅動要求重設或改變緩衝大小時重新開啟裝置（播放器在原位置繼續）。啟動後 0.5 s 內的一次會被忽略（驅動啟動時常會送一次）。10 秒內超過 3 次視為失敗。取樣率改變則停止播放並說明原因。
- 驅動可能在自己的音訊執行緒呼叫 asioMessage / sampleRateDidChange，所以這兩個回呼不配置記憶體、不阻塞：記錄訊息以 `vsnprintf` 寫進預先配置的固定欄位（`try_lock` 取得；播放器正在讀取或已滿 64 則時捨棄並計數，下次 `take_notes()` 回報捨棄幾則），失敗原因寫進固定緩衝區後才以 release 設定 `failed()`，延遲是原子變數。
- 驅動 DLL 永不卸載：經由 COM 釋放時會在 CoUninitialize 卸載，若驅動的執行緒還在收尾，就會在已釋放的程式碼中執行而當機。
- DSD 模式：環不夠時以 DSD 靜音（0x69）填補，不是 0。
- 原生 DSD 結束時不切回 PCM（有些 DAC 在原生 DSD → PCM 的切換會爆音，而且關閉時跑一段 PCM 工作階段後，下一次原生 DSD 可能沒有聲音）：DAC 留在 DSD 模式，下一個 PCM 或 DoP 串流在自己的取樣率先送 0.2 s 全為 0 的 PCM，讓 DAC 離開 DSD 模式（有些 DAC 在原生 DSD 之後直接接 DoP 會在某一聲道持續發出嗶聲）。同一個程序裡的第一個 PCM / DoP 串流也這樣開始（不知道 DAC 之前停在什麼模式）。
- 開始前兩個 buffer 都先填靜音（PCM 為 0、DSD 為 0x69），驅動程式在第一次 callback 之前播放的那一段不會是 createBuffers 留下的內容。
- 原生 DSD 在播完之前被停止時，先讓 DSD 靜音（0x69）送到 DAC 再停止，不在音樂中間切斷。
- 需要建置時有 Steinberg ASIO SDK（`third_party/asiosdk`，`scripts/fetch_asiosdk.ps1`）；沒有時 `open()` 回報失敗，其他輸出不受影響。

## 裝置列表（`devices.h`）

- `list_wasapi_devices(probe)`：作用中的 render 端點（ID、名稱、預設、混音器格式）；probe 時以 `IsFormatSupported` 測試獨占模式下 44.1k–768k × 16/24/32-bit 的立體聲格式。
- `list_asio_drivers()`：`HKLM\SOFTWARE\ASIO` 下安裝的驅動（不需要 SDK）。
- CLI：`daphne devices [--probe]`。

## 驗證

| 項目 | 結果 |
|---|---|
| `daphne devices`：列出 6 個 WASAPI 端點與 2 個 ASIO 驅動 | 正確 |
| WASAPI 獨占，44.1 kHz、24 / 16-bit 隨機樣本（含滿刻度極值）錄回比對（`verify/live.py` 第 1 節） | 每個樣本都相同（bit-perfect） |
| WASAPI 獨占 176.4 kHz：無縫跨取樣率、ReplayGain 換軌、DoP 傳輸（第 2–4 節） | 全部通過，underrun 0 |
| ASIO | 編譯通過；ASIO 只能透過實體裝置測試（會發出聲音），尚待實機聆聽確認 |
