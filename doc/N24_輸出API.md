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

統計（`OutputStats`，原子變數）：回呼數、underrun、WASAPI 事件逾時（> 50 ms 沒有裝置事件）、最長回呼間隔、ASIO 遲到回呼（相鄰回呼間隔 > 1.5 個緩衝區週期）。

## WASAPI

| 模式 | 做法 |
|---|---|
| 獨占 | 事件驅動（`AUDCLNT_STREAMFLAGS_EVENTCALLBACK`）。格式搜尋順序：取樣率為要求值、同家族逐次減半，再試 192k / 96k / 48k / 44.1k；樣本格式為 int32、24-in-32、float32、packed24、int16。每種組合先以 `IsFormatSupported` 檢查 |
| 共享 | 系統混音器格式；取樣率不同時請 Windows 轉換（`AUTOCONVERTPCM`），以正確速度播放（共享模式本來就不是 bit-perfect） |

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
- 驅動 DLL 永不卸載：經由 COM 釋放時會在 CoUninitialize 卸載，若驅動的執行緒還在收尾，就會在已釋放的程式碼中執行而當機。
- DSD 模式：環不夠時以 DSD 靜音（0x69）填補，不是 0。
- 原生 DSD 之後，`close()` 先跑 0.2 s、44.1 kHz、全為 0 的 PCM 工作階段，讓 DAC 離開 DSD 模式（有些 DAC 否則會在某一聲道持續發出嗶聲）。
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
