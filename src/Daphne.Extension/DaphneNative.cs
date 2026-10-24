using System;
using System.Buffers;
using System.IO;
using System.Runtime.InteropServices;
using System.Text;

namespace DaphneMiku;

/// <summary>P/Invoke into daphne_core.dll (capi/daphne_core.h), loaded from the extension's folder.</summary>
static class DaphneNative
{
    const string Dll = "daphne_core";

    public const int Ok = 0, ErrArg = -1, ErrSettings = -2, ErrFile = -3, ErrDevice = -4, ErrBuffer = -5;

    static DaphneNative()
    {
        // ext\daphne\daphne_core.dll (MIKU loads extensions in their own AssemblyLoadContext; the FFmpeg DLLs next to it
        // are found because the library is loaded by full path)
        NativeLibrary.SetDllImportResolver(typeof(DaphneNative).Assembly, (name, asm, path) =>
        {
            if (name != Dll) return IntPtr.Zero;
            string dir = Path.GetDirectoryName(typeof(DaphneNative).Assembly.Location);
            return NativeLibrary.Load(Path.Combine(dir, Dll + ".dll"));
        });
    }

    [DllImport(Dll)] public static extern int dc_abi_version();
    [DllImport(Dll)] static extern IntPtr dc_version();
    [DllImport(Dll)] public static extern DaphnePlayerHandle dc_create();
    [DllImport(Dll)] internal static extern void dc_destroy(IntPtr p);
    [DllImport(Dll, CharSet = CharSet.Unicode)] static extern int dc_load(DaphnePlayerHandle p, string path, byte[] settings, double seconds, int play, byte[] err, int errLen);
    [DllImport(Dll)] static extern int dc_apply(DaphnePlayerHandle p, byte[] settings, byte[] err, int errLen);
    [DllImport(Dll)] public static extern void dc_pause(DaphnePlayerHandle p, int paused);
    [DllImport(Dll)] public static extern void dc_seek(DaphnePlayerHandle p, double seconds);
    [DllImport(Dll, CharSet = CharSet.Unicode)] public static extern void dc_set_next(DaphnePlayerHandle p, string path);
    // the next track with its own headroom (ReplayGain per track), applied where it starts decoding; NaN keeps it
    [DllImport(Dll, CharSet = CharSet.Unicode)] public static extern void dc_set_next_gain(DaphnePlayerHandle p, string path, double headroomDb);
    [DllImport(Dll)] public static extern void dc_stop(DaphnePlayerHandle p);
    [DllImport(Dll)] static extern int dc_state(DaphnePlayerHandle p, byte[] json, int len);
    [DllImport(Dll)] static extern int dc_poll_event(DaphnePlayerHandle p, byte[] json, int len);
    [DllImport(Dll)] public static extern void dc_meter(DaphnePlayerHandle p, out double l, out double r);
    [DllImport(Dll)] public static extern double dc_design(DaphnePlayerHandle p);
    // live input (YouTube Music): MIKU's shared ring (LiveBus) and the page's sample rate
    [DllImport(Dll)] public static extern void dc_live_bus(IntPtr ringBase, int capFrames, int rate);
    [DllImport(Dll)] public static extern void dc_live_rate(int rate);
    [DllImport(Dll)] static extern int dc_devices(byte[] json, int len);
    [DllImport(Dll, CharSet = CharSet.Unicode)] static extern int dc_filter_cache_dir(char[] dir, int len);
    [DllImport(Dll)] static extern long dc_filter_cache_prune(byte[] settings, int limitMb);

    /// <summary>Keeps the filter cache at most limitMb (0: as small as it gets), least recently used first, never the
    /// filters the settings use. Returns the bytes left, or -1.</summary>
    public static long PruneFilterCache(string settingsJson, int limitMb) => dc_filter_cache_prune(Utf8z(settingsJson), limitMb);

    /// <summary>The filter cache folder, or null when the cache is disabled.</summary>
    public static string FilterCacheDir()
    {
        var b = new char[1024];
        int n = dc_filter_cache_dir(b, b.Length);
        return n > 0 ? new string(b, 0, n) : null;
    }

    static byte[] Utf8z(string s) => Encoding.UTF8.GetBytes((s ?? "") + "\0");
    static string FromUtf8z(byte[] b) { int n = Array.IndexOf(b, (byte)0); return Encoding.UTF8.GetString(b, 0, n < 0 ? b.Length : n); }

    public static string Version => Marshal.PtrToStringAnsi(dc_version());

    public static int Load(DaphnePlayerHandle p, string path, string settingsJson, double seconds, bool play, out string error)
    {
        var err = new byte[4096];
        int r = dc_load(p, path, Utf8z(settingsJson), seconds, play ? 1 : 0, err, err.Length);
        error = FromUtf8z(err);
        return r;
    }

    public static int Apply(DaphnePlayerHandle p, string settingsJson, out string error)
    {
        var err = new byte[4096];
        int r = dc_apply(p, Utf8z(settingsJson), err, err.Length);
        error = FromUtf8z(err);
        return r;
    }

    /// <summary>Calls a JSON-returning function, growing the buffer when it is too small.</summary>
    static string Json(Func<byte[], int> f)
    {
        for (int size = 1 << 14; size <= 1 << 22; size <<= 2)
        {
            var b = ArrayPool<byte>.Shared.Rent(size);
            try
            {
                int r = f(b);
                if (r == ErrBuffer) continue;
                return r > 0 ? Encoding.UTF8.GetString(b, 0, r) : null;
            }
            finally { ArrayPool<byte>.Shared.Return(b); }
        }
        return null;
    }

    public static string State(DaphnePlayerHandle p) => Json(b => dc_state(p, b, b.Length));
    public static string PollEvent(DaphnePlayerHandle p) => Json(b => dc_poll_event(p, b, b.Length));
    public static string Devices() => Json(b => dc_devices(b, b.Length));
}

// The P/Invoke marshaler holds a reference for the whole native call. Dispose therefore
// cannot destroy the player while another thread is still loading/polling/metering it.
sealed class DaphnePlayerHandle : SafeHandle
{
    public DaphnePlayerHandle() : base(IntPtr.Zero, ownsHandle: true) { }
    public override bool IsInvalid => handle == IntPtr.Zero;
    protected override bool ReleaseHandle()
    {
        DaphneNative.dc_destroy(handle);
        return true;
    }
}
