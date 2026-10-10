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

## 驗證

### 單元測試（`tests/test_decoders.cpp`）

| 測試 | 結果 |
|---|---|
| WAV 16 / 24 / 32-bit 整數：`x · 2^(N-1)` 與寫入樣本完全相同，frames 與 bits 正確 | 通過 |
| WAV float64（96 kHz，含 1e-12 級細節）：逐位元相同，`lossless_int = false` | 通過 |
| WAV 搜尋到 frame 4321：第一個讀出的樣本即目標樣本 | 通過 |

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

接點誤差 ≤ 0 dB 表示接點不比曲中其他位置差（有損編碼在單獨編碼的檔案頭尾本來就有邊緣誤差）。AAC 這裡比 FFmpeg 命令列好：命令列不套用 iTunSMPB 的尾端裁切，多出 1232 frames 的補零。
