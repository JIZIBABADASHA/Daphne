# N2 PCM 解碼器

## WAV / AIFF / AIFC（`decoder_wav.cpp`，dr_wav）

| 來源 | 讀法 | 精度 |
|---|---|---|
| 整數 PCM 8–32 bit、A-law、μ-law、ADPCM | `drwav_read_pcm_frames_s32`（左對齊 s32）× 2^-31 | 精確 |
| float32 | `drwav_read_pcm_frames_f32` → double | 精確 |
| float64 | `drwav_read_pcm_frames`（原生位元組序）**直接讀進輸出緩衝區** | 位元相同 |

- float64 不走 dr_wav 的 f32 路徑（會被捨入成單精度）。`drwav_read_pcm_frames` 依容器把大端（AIFF）樣本轉成原生位元組序；不可用 `_le` 版本，它只回傳原始位元組，大端 AIFF 的 float64 會變成亂碼。
- 效能：float64 沒有中間緩衝區與複製；整數與 float32 使用重複利用的成員緩衝區，不在每次 `read` 配置。
- `channel_mask` 取自 WAVE_FORMAT_EXTENSIBLE，供多聲道降混使用。
- 只有 `DR_WAVE_FORMAT_PCM` 標記 `lossless_int` 與 `bits`；壓縮格式（A-law 等）與浮點視為非整數來源。

## FLAC（`decoder_flac.cpp`，dr_flac）

- `drflac_read_pcm_frames_s32` × 2^-31，所有位元深度（含 32 bit）精確。
- 停用 Ogg FLAC（`DR_FLAC_NO_OGG`）：OGA 交給 FFmpeg。
- 搜尋使用 dr_flac 的 seek table / 二分搜尋，落點為精確樣本。

## MP3 / MP2 / MP1（`decoder_mp3.cpp`，minimp3_ex）

- 浮點輸出（`MINIMP3_FLOAT_OUTPUT`）：保留完整精度，超過 0 dBFS 的樣本不被截斷（不採用 int16 輸出）。
- minimp3_ex 讀取 LAME / Info 標籤，裁掉編碼器延遲與尾端補零，達成無縫。
- `MP3D_SEEK_TO_SAMPLE`：精確到樣本的搜尋。

## FFmpeg（`decoder_ffmpeg.cpp`，`DAPHNE_HAVE_FFMPEG`）

- avformat → 最佳音訊串流 → avcodec → swresample 轉為交錯 float64，取樣率與聲道配置維持來源。
- 無損整數編碼（ALAC、APE、WavPack、TTA、其他容器內的 FLAC、PCM）在 float64 中精確，標記 `lossless_int`。
- **MP4 無縫**：FFmpeg 已去掉編碼器延遲；尾端補零依 iTunSMPB 標籤的長度裁切，沒有標籤時使用串流長度（edit list）。位置從串流起始時間算起，frame 0 即第一個可聽樣本。Ogg（Vorbis / Opus）與 MP3 的裁切由 FFmpeg 處理。
- **搜尋**：有損編碼的每一框依賴前一框（MDCT 重疊），因此提前 4096 frames 起解，等解碼器穩定後再丟棄目標之前的樣本；靠近開頭時直接從頭解，以免跳過解碼器需要的延遲封包。
- 沒有 FFmpeg 時改由 stub 回傳「未啟用」錯誤，其他格式不受影響。

## 即時輸入（`decoder_live.cpp`，`live_source.h`）

主程式與一個它無法控制的音訊來源（YouTube Music 網頁的 Web Audio 擷取）共用的環形緩衝，以路徑 `live:` 當成解碼器開啟。

- 版面（`dc_live_bus`）：uint32 寫入位置（frame，會繞回）｜12 位元組保留｜float32 立體聲。來源先寫樣本、再寫位置。
- 來源有自己的時鐘，而且是一陣一陣送：在環形緩衝裡保持約 0.3 秒的抖動緩衝，以讀取速率的微調（最多 ±0.5%，平滑後實際遠小於 0.3%）維持在那裡。正常播放時不丟也不重複樣本。
- **內插**：讀取位置落在兩個樣本之間，以 64 taps 的 Kaiser 窗 sinc（512 個相位，相鄰相位線性內插；截止 0.45 fs、β = 10）計算。48 kHz 下頻響平直到 19 kHz（20 kHz −0.1 dB），雜散成分約 −125 dB。
  - 為什麼不用短內插：速率修正再小，讀取位置的小數部分還是會掃過 0 到 1 的所有值，所以誤差跟修正量無關。原本的 4 點 Hermite 在 10 kHz 只有 −45 dB、15 kHz −32 dB（同一個量測方法）。
  - 這個內插器向前看 32 個樣本，延遲約 0.7 ms，相對於 0.3 秒的抖動緩衝可以忽略。
- **例外狀況都以 5 ms 淡入淡出處理，不直接跳**：
  - 開始：還沒累積到 0.3 秒時立刻回傳靜音（播放器預填環形緩衝），累積後淡入。
  - 來源跟不上（starve）：等來源最多 20 ms，仍然沒有就用預留的最後 5 ms 淡出（正常讀取時這 5 ms 不會被讀掉），之後每次回傳 5 ms 靜音，直到再累積 0.3 秒才淡入。播放器不會被卡住。starve 次數以事件計，不是每次靜音都算。
  - 累積超過 1.2 秒（播放器暫停過）：先淡出，從靠近寫入端的位置重新開始，再淡入。
  - 被寫入端追過、頁面重新載入：原本的資料已被覆蓋，無法淡出；從靠近寫入端的位置重新開始並淡入。
- 寫入位置以有號差值比較，2³² frame（48 kHz 下約 25 小時）繞回時不會被當成重新載入。
- `dc_live_bus(NULL)` 會等正在進行的讀取離開共用記憶體才返回，之後主程式就可以釋放它。
- 輸出：開啟時來源的取樣率、2 聲道、長度未知（frames = −1）、不能搜尋（`seek` 只改位置計數，串流從現在繼續）。
- `dc_state` 在播放即時輸入時有 `live`：環形緩衝裡的秒數、已讀秒數、重新同步與 starve 次數、速率修正。從來源到 DAC 的延遲 = `fill + read − position + latency_ms / 1000`。

## 驗證

### 單元測試（`tests/test_decoders.cpp`）

| 測試 | 結果 |
|---|---|
| WAV 16 / 24 / 32-bit 整數：`x · 2^(N-1)` 與寫入樣本完全相同，frames 與 bits 正確 | 通過 |
| WAV float64（96 kHz，含 1e-12 級細節）：逐位元相同，`lossless_int = false` | 通過 |
| WAV 搜尋到 frame 4321：第一個讀出的樣本即目標樣本 | 通過 |

### 單元測試（`tests/test_live.cpp`）

| 測試 | 結果 |
|---|---|
| 來源開始前立刻回傳靜音；來源領先 0.5 秒後讀寫交替：1 kHz 正弦相鄰樣本差不超過理論值、兩聲道分開、沒有重新同步或 starve | 通過 |
| 被寫入端追過時重新同步一次；資料讀完後淡出（最後一個樣本接近 0），再等待約 20 ms 回傳 240 frames（5 ms）靜音；starve 只計一次 | 通過 |
| 來源比讀取端快 0.1%（速率修正持續作用）：1 / 10 / 15 kHz 正弦，每次讀取的輸出（速率固定）去掉擬合的正弦後，殘差 −125 / −124 / −125 dB（門檻 −90 dB；Hermite 為 10 kHz −45、15 kHz −32 dB，不通過） | 通過 |
| 累積超過 1.2 秒：DC 0.5 降到 0 再回到 0.5，相鄰樣本差不超過淡入淡出的一步 | 通過 |
| 寫入位置從 2³² 前約 0.6 秒開始、讀過繞回：沒有重新同步、沒有 starve | 通過 |
| 讀取中解除並立即釋放共用記憶體，重複 200 次：AddressSanitizer 下沒有釋放後使用（原本的版本會被抓到 heap-use-after-free） | 通過 |
| 沒有共用緩衝時無法開啟 | 通過 |

### 端到端（`verify/decoders.py` 第 1、2、5 節）

| 項目 | 結果 |
|---|---|
| WAV / AIFF 16 / 24 / 32-bit、FLAC 16 / 24-bit（隨機滿刻度樣本）→ 渲染回原位元深度 | 全部逐樣本相同 |
| FLAC 16 / 24-bit `decode-test`：解碼串流的 MD5 與 STREAMINFO MD5 相符 | OK |
| float64 WAV / AIFF（大端） | 正確（渲染為 float32 後與來源 float32 相同；WAV 的位元相同由單元測試確認） |

無縫（一段訊號在非編碼框邊界處切成兩軌、分別編碼、解碼後接回）：

| 編碼 | 長度誤差 | 接點誤差 vs 其他位置 | FFmpeg 自身解碼 |
|---|---|---|---|
| FLAC | 0 | +0.0 dB | 0, +0.0 dB |
| MP3 320k（LAME） | 0 | −0.7 dB | 0, −0.7 dB |
| AAC 256k（M4A） | 0 | −2.7 dB | **+1232**, −3.6 dB |
| Vorbis q6 | 0 | −2.3 dB | 0, −2.3 dB |
| Opus 192k | 0 | −0.6 dB | 0, −0.6 dB |

即時輸入實機（測試程式扮演網頁：以系統時鐘、每 5–25 ms 一陣寫入 48 kHz 1 kHz 正弦；Daphne 經虛擬音效線播放，以裝置取樣率錄回，各 10 秒）：

| 裝置 | 二階差分最大值（相對乾淨正弦） | 零值段 | underrun | 重新同步 | 來源 → DAC 延遲 |
|---|---|---|---|---|---|
| 96 kHz（×2 升頻） | 1.00 倍 | 無 | 0 | 0 | 約 1.3 s |
| 48 kHz | 1.00 倍 | 無 | 0 | 0 | 約 1.3 s |

延遲是抖動緩衝（約 0.35 s）、處理鏈與播放器的環形緩衝（約 0.7 s）與裝置緩衝的和。

接點誤差 ≤ 0 dB 表示接點不比曲中其他位置差（有損編碼在單獨編碼的檔案頭尾本來就有邊緣誤差）。AAC 這裡比 FFmpeg 命令列好：命令列不套用 iTunSMPB 的尾端裁切，多出 1232 frames 的補零。
