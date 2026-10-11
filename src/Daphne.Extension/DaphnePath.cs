using System;
using System.Collections.Generic;
using System.Globalization;
using System.Linq;
using System.Text.Json;
using Miku;

namespace DaphneMiku;

/// <summary>
/// Daphne's own signal path card (SignalInfo.Custom, drawn by web/daphne.js): sections 來源 / DSP / 輸出, each row a
/// one-line summary with its details under it (the page folds a row that has details, or does not fit on one line).
/// The DSP rows follow the core's processing chain (report "chain"), in its order.
/// </summary>
public sealed partial class DaphneEngine
{
    /// <summary>
    /// One row: label, the one-line summary, whether it changes the samples (the pink dot), the details (folded), and
    /// decoder: shown under the summary all the time ("daphne": 解碼 with Daphne's mark).
    /// </summary>
    sealed record PathRow(string k, string v, bool mod, string[] more, string decoder = null);

    static PathRow Row(string k, string v, bool mod, params string[] more) =>
        new(k, v, mod, more.Where(x => !string.IsNullOrEmpty(x)).ToArray());

    static string Khz(int r) => (r / 1000.0).ToString("0.###", CultureInfo.InvariantCulture);
    static string Mhz(int r) => (r / 1e6).ToString("0.####", CultureInfo.InvariantCulture);
    static string Db(double d) => (d > 0 ? "+" : "") + d.ToString("0.0", CultureInfo.InvariantCulture) + " dB";
    static string DsdName(int clock) => "DSD" + clock / (clock % 44100 == 0 ? 44100 : 48000);
    static string Hz(double f) => f >= 1000 ? (f / 1000).ToString("0.##", CultureInfo.InvariantCulture) + " kHz" : f.ToString("0", CultureInfo.InvariantCulture) + " Hz";

    string PhaseText() => _opts.Phase switch
    {
        "linear" => "線性相位",
        "minimum" => "最小相位",
        "custom" => $"自訂相位（α {_opts.PhaseCustom.ToString("0.00", CultureInfo.InvariantCulture)}）",
        _ => "中間相位",
    };

    /// <summary>The upsampling filter's details (PCM 升頻, and the pre-filter before the DSD modulator).</summary>
    string[] FilterLines(bool apodize) => new[]
    {
        $"濾波器：{_opts.Taps.ToString("N0", CultureInfo.InvariantCulture)} taps · {PhaseText()}",
        apodize ? "去除錄音振鈴（apodize）：開" : null,
        _opts.DoubleDouble ? "雙倍精度摺積（double-double）：開" : null,
    };

    static string BandName(string type) => type switch
    {
        "LSC" => "低頻架", "HSC" => "高頻架", "LP" => "低通", "HP" => "高通", _ => "峰值",
    };

    /// <summary>The 音量 row when the core's gain stage does nothing (0 dB, fixed, hardware, or DSD sent directly).</summary>
    PathRow VolumeRow(string volumeMode, bool dsdFile)
    {
        return volumeMode switch
        {
            "hardware" => Row("音量", "DAC 硬體音量 · " + Db(_s.VolumeDb), false, "音量由 DAC 調整，送出的樣本不變"),
            "none" => Row("音量", "無（請使用 DAC 旋鈕）", false, dsdFile ? "DSD 直送不經過數位音量" : null),
            "digital" => Row("音量", "數位音量 · 0 dB（不處理）", false),
            _ => Row("音量", "固定 0 dB", false, "不調整數位增益"),
        };
    }

    /// <summary>The card's sections and its notes (warnings).</summary>
    object BuildPath(Track t, JsonElement src, JsonElement dev, List<string> chain, bool isDsd, bool nativeDsd, bool dop,
                     int srcRate, int devRate, string mode, string format, string volumeMode, double rg, List<string> notes)
    {
        var o = _opts;
        bool dsdOut = nativeDsd || dop;
        bool dsdFile = isDsd && dsdOut;            // a DSD file sent as DSD
        bool dsdPcm = isDsd && !dsdOut;            // a DSD file converted to PCM
        int pcmRate = src.GetProperty("rate").GetInt32();   // where the PCM chain starts (DSD → PCM: the converted rate)
        int channels = src.GetProperty("channels").GetInt32();
        int clock = nativeDsd ? devRate : dop ? devRate * 16 : 0;   // the DSD rate sent
        bool upsampler = SettingsMap.Upsampler(_s, o) != "off";
        var sections = new List<object>();

        // ── 來源
        string srcText = isDsd ? $"{t.Codec} • {DsdName(srcRate)} • {Mhz(srcRate)} MHz"
                               : $"{t.Codec} • {(t.Bits > 0 ? t.Bits + "-bit / " : "")}{Khz(srcRate)} kHz";
        sections.Add(new
        {
            title = "來源",
            rows = new[]
            {
                Row("檔案", srcText, false, channels != 2 ? $"聲道：{channels}" : null,
                    t.IsLossy ? "有損壓縮來源：壓縮時已丟棄部分聲音資訊，無法還原" : null) with { decoder = "daphne" },
            },
        });

        // ── DSP (the core's chain, in its order)
        var proc = new List<PathRow>();
        if (dsdPcm)
            proc.Add(Row("DSD 轉 PCM", $"{DsdName(srcRate)} → PCM {Khz(pcmRate)} kHz", true,
                $"低通濾波：{o.DsdPcmLowpass} kHz（濾掉 DSD 的超音波噪聲）",
                "增益：+6 dB（SACD 0 dB = 0 dBFS，與 PCM 同音量）",
                o.DsdPcmLimiter ? "限幅器：開（超過滿刻度的峰值壓回，不削波）" : "限幅器：關（超過滿刻度時削波）"));
        if (chain.Contains("volume"))
        {
            bool digital = _s.VolumeMode == "digital";
            var parts = new List<string>();
            if (digital) parts.Add(_s.Muted ? "靜音" : "數位音量 " + Db(_s.VolumeDb));
            if (Math.Abs(rg) > 1e-9) parts.Add("ReplayGain " + Db(rg));
            proc.Add(Row("音量", parts.Count > 0 ? string.Join(" · ", parts) : "增益", true,
                digital ? "數位音量：" + (_s.Muted ? "靜音" : Db(_s.VolumeDb)) : null,
                Math.Abs(rg) > 1e-9 ? $"ReplayGain（{(_s.ReplayGain == "album" ? "依專輯" : "依曲目")}）：{Db(rg)}，只在峰值允許的範圍內提高" : null,
                "64-bit 浮點運算，調整時平滑過渡"));
        }
        var d = _s.Dsp;
        if (chain.Contains("crossfeed") && d?.Crossfeed != null)
            proc.Add(Row("Crossfeed", $"{Hz(d.Crossfeed.Fc)} · {d.Crossfeed.Feed.ToString("0.0", CultureInfo.InvariantCulture)} dB", true,
                $"截止頻率：{Hz(d.Crossfeed.Fc)}（低於此頻率的聲音也送到另一耳）",
                $"交叉量：{d.Crossfeed.Feed.ToString("0.0", CultureInfo.InvariantCulture)} dB（直接音比交叉音大的幅度）"));
        if (chain.Contains("eq") && d != null)
        {
            var on = d.Bands.Where(b => b.On).ToList();
            var lines = new List<string> { "前級：" + (d.AutoPreamp ? "自動" : Db(Math.Min(0, d.PreampDb))) };
            lines.AddRange(on.Select(b => b.Type is "LP" or "HP"
                ? $"{BandName(b.Type)} {Hz(b.Fc)} · Q {b.Q.ToString("0.##", CultureInfo.InvariantCulture)}"
                : $"{BandName(b.Type)} {Hz(b.Fc)} · {Db(b.Gain)} · Q {b.Q.ToString("0.##", CultureInfo.InvariantCulture)}"));
            proc.Add(Row("EQ", $"{on.Count} 段" + (string.IsNullOrEmpty(d.PresetName) ? "" : " · " + d.PresetName), true, lines.ToArray()));
        }
        if (chain.Contains("resampler"))
            proc.Add(Row("升頻", devRate == pcmRate ? "1 倍 · 只去除錄音振鈴" : $"{Khz(pcmRate)} → {Khz(devRate)} kHz", true,
                FilterLines(o.Apodize && upsampler)));
        if (chain.Contains("modulator pre-filter") || chain.Contains("modulator limiter"))
        {
            int modIn = pcmRate % 44100 == 0 ? 705600 : 768000;
            var lines = new List<string> { $"前級升頻：{Khz(pcmRate)} → {Khz(modIn)} kHz" };
            lines.AddRange(FilterLines(o.Apodize).Where(x => x != null));
            lines.Add("限幅器：峰值保持在調變器的穩定範圍內");
            if (clock > 0) lines.Add($"調變器：{o.Order} 階 Δ-Σ → {DsdName(clock)}（{Mhz(clock)} MHz）");
            proc.Add(Row("轉成 DSD", (clock > 0 ? DsdName(clock) + " · " : "") + $"{o.Order} 階調變器", true, lines.ToArray()));
        }
        if (chain.Contains("quantizer"))
        {
            int bits = dev.GetProperty("bits").GetInt32();
            proc.Add(Row("量化", $"TPDF 抖動 → {bits}-bit", true, $"從 64-bit 浮點量化到裝置的 {bits}-bit，加 TPDF 抖動"));
        }
        if (proc.Count == 0)
            proc.Add(Row("DSP", dsdFile ? "無：DSD 原樣送出" : "無：樣本原樣送出", false));
        sections.Add(new { title = "DSP", rows = proc });

        // ── 輸出
        var outRows = new List<PathRow>();
        if (!chain.Contains("volume")) outRows.Add(VolumeRow(volumeMode, dsdFile));
        double latency = _report.TryGetProperty("latency_ms", out var lm) ? lm.GetDouble() : 0;
        long underruns = _report.TryGetProperty("underruns", out var un) ? un.GetInt64() : 0;
        outRows.Add(Row("裝置", $"{mode} · {_report.GetProperty("device_name").GetString()}", false,
            latency > 0 ? $"輸出延遲：約 {latency:0} ms" : null, underruns > 0 ? $"欠載：{underruns} 次" : null));
        if (nativeDsd)
            outRows.Add(Row("格式", $"原生 DSD · {DsdName(clock)}（{Mhz(clock)} MHz）", false, "以 ASIO 原生 DSD 送到 DAC"));
        else if (dop)
            outRows.Add(Row("格式", $"DoP · {format} / {Khz(devRate)} kHz", false,
                $"{DsdName(clock)} 包在 PCM 裡送出（DoP 標記），DAC 解開後以 DSD 播放"));
        else
            outRows.Add(Row("格式", $"{format} / {Khz(devRate)} kHz", false));
        sections.Add(new { title = "輸出", rows = outRows });

        return new { ext = "daphne", sections, notes };
    }
}
