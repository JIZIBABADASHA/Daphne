# N20 DSD 封裝

`core/include/daphne/engine/dsd_output.h`、`core/src/dsd_output.cpp`

DSD 位元組（調變器輸出，或 DSD 檔直送）依傳輸方式封裝成裝置的位元組格式。

## 來源

| 初始化 | 來源 | 聲道 |
|---|---|---|
| `init(pcm_rate, osr, order, dev, t)` | `dsd` 核心：705.6 / 768 kHz 的 PCM 經調變器（N19），DSD 時脈 44100 × osr | 1–2 |
| `init_direct(clock, dev, t)` | DSD 檔直送（N3 的 `DsdFile`），不經任何處理 | 1–8 |

## 傳輸

### DoP（DSD over PCM）

- 每聲道每 frame 一個 24-bit 字：標記（0x05 / 0xFA，每 frame 交替）+ 16 個 DSD bit，最早的 bit 在最前。
- 裝置取樣率 = DSD 時脈 / 16（DSD64 → 176.4 kHz，DSD128 → 352.8 kHz）。
- 直接寫進整數容器，不縮放（標記必須精確）：

| 容器 | 排列 |
|---|---|
| `Int32` / `Int24In32` | 24-bit 字左對齊（低位元組為 0） |
| `Packed24` | 3 位元組 |
| `Int32Low`（≥ 24 有效位元） | 24-bit 字右對齊 |

- 需要 24 位元以上的整數格式（`dop_capable`）；16-bit 或浮點格式不能傳 DoP。
- 不足一個 DoP frame 的位元組留到下一次，標記序列跨呼叫連續。

### 原生 DSD（ASIO）

- 每聲道每 frame 一個位元組（8 個 DSD bit）。
- `Dsd8Msb` 原樣送出；`Dsd8Lsb` 以 256 項查表做位元反轉。

## 靜音

`idle(frames)` 送出 DSD 靜音位元組 0x69，DoP 標記照常交替：串流結束或重新開始時 DAC 保持在 DSD 模式，不會切回 PCM 造成爆音。

播放開始時（以及裝置重開時）先送 `output.dsd_lead_in_ms`（預設 100 ms）的靜音再送音樂：DAC 從 PCM 切到 DSD 時會靜音一段時間來鎖定，這段前導避免切掉曲首。在播放器（N4）裡呼叫 `idle()`。

## 效能

- `pack()` 先算出可輸出的 frame 數，一次預留輸出空間。
- 已消耗的輸入超過 64 KB 才整理緩衝區（不在每次呼叫搬移資料）。

## 驗證（`tests/test_dsd_output.cpp`）

| 測試 | 結果 |
|---|---|
| DoP、Int32、DSD64、44.1 kHz 輸入：4096 frames → 4096 × 4 個 DoP frames；每 frame 標記 0x05 / 0xFA 交替、兩聲道相同、低位元組 0 | 通過 |
| DoP 酬載 = 同設定調變器的輸出位元組，舊的在中間位元組、新的在低位元組 | 通過 |
| `idle(3)`：0x69 酬載，標記延續序列（0x05 → 0xFA） | 通過 |
| 16-bit 格式不能初始化 DoP；20 有效位元的 Int32Low 不能 | 通過 |
| Packed24 / Int24In32 / Int32Low(24) 的 DoP 位元組排列 | 通過 |
| 原生 DSD：MSB 格式原樣；LSB 格式逐位元組反轉 | 通過 |
| 原生傳輸配整數格式 → 錯誤；直送 6 聲道 → 可以；描述字串以 DSD64 開頭 | 通過 |
