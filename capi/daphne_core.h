/* daphne_core C ABI: the player for host applications.
 * Calls only go host -> library; the library never calls back into the host. Events and state are
 * polled as JSON (UTF-8). Errors are return codes plus an optional message buffer; no exception
 * crosses this boundary. Every function may be called from any thread. */
#ifndef DAPHNE_CORE_H
#define DAPHNE_CORE_H

#include <stdint.h>
#include <wchar.h>

#ifdef _WIN32
#  ifdef DC_BUILDING_DLL
#    define DC_API __declspec(dllexport)
#  else
#    define DC_API __declspec(dllimport)
#  endif
#else
#  define DC_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define DC_OK 0
#define DC_ERR_ARG -1
#define DC_ERR_SETTINGS -2   /* the settings JSON has errors (message lists them) */
#define DC_ERR_FILE -3       /* the file could not be opened / decoded */
#define DC_ERR_DEVICE -4     /* the output device could not be opened / started */
#define DC_ERR_BUFFER -5     /* the output buffer is too small (the return value of *_json functions) */

typedef struct dc_player dc_player;

DC_API int dc_abi_version(void);
DC_API const char* dc_version(void);

DC_API dc_player* dc_create(void);
/* Stops playback and frees the player. */
DC_API void dc_destroy(dc_player* p);

/* Opens path (UTF-16) with the settings (settings JSON, see settings.h; output.api / output.device
 * choose the device) and starts at `seconds`, playing or paused. Blocks until playback has started or
 * failed (a long filter design is reported through dc_state "design" meanwhile). A playing track is
 * stopped first. */
DC_API int dc_load(dc_player* p, const wchar_t* path, const char* settings_json, double seconds, int play, char* err, int err_len);
/* New settings for the playing track (in place, rebuild + crossfade, or device restart as needed). */
DC_API int dc_apply(dc_player* p, const char* settings_json, char* err, int err_len);
DC_API void dc_pause(dc_player* p, int paused);
DC_API void dc_seek(dc_player* p, double seconds);
/* Gapless: the track to continue with when this one ends (same output format only; another sample rate
 * when it plays on the same output); NULL / "" clears. */
DC_API void dc_set_next(dc_player* p, const wchar_t* path);
/* dc_set_next with the next track's own headroom (dB, settings "headroom_db": ReplayGain per track), applied where
 * its first sample is decoded; NaN keeps the current one. */
DC_API void dc_set_next_gain(dc_player* p, const wchar_t* path, double headroom_db);
/* Stops playback (DSD: the DAC is returned to PCM first) and waits for it. */
DC_API void dc_stop(dc_player* p);

/* State snapshot as JSON:
 *   {"state": "idle|playing|paused|ended|failed", "position": s, "duration": s, "track": n, "design": 0..1,
 *    "error": "...", "report": {"source": {...}, "device": {...}, "device_name": "...", "bit_perfect": b,
 *    "reason": "...", "filter": "...", "chain": [...], "latency_ms": x, "underruns": n}}
 * "track" counts gapless transitions since dc_load; "asio_rates": PCM rates the open ASIO driver accepts (hosts use
 * them instead of opening the driver a second time); "debug": internal state for logs.
 * Returns the length, or DC_ERR_BUFFER. */
DC_API int dc_state(dc_player* p, char* json, int len);
/* The oldest queued event as JSON, removing it; returns its length, 0 when there is none, or
 * DC_ERR_BUFFER (the event stays queued).
 *   {"type": "started"}            playback started (again after a device restart)
 *   {"type": "track"}              gapless: the next track (dc_set_next) is now heard
 *   {"type": "changed"}            the active filter / processing chain changed (the report in dc_state is new)
 *   {"type": "ended"}              the end of the last track was heard
 *   {"type": "failed", "error": "..."}
 *   {"type": "info", "text": "..."} a settings change was applied, a filter is being designed, ... */
DC_API int dc_poll_event(dc_player* p, char* json, int len);
/* Filter design progress 0..1 (1 = nothing being designed); never blocks, also during dc_load. */
DC_API double dc_design(dc_player* p);
/* Peak level heard since the last call, 0..1 (PCM). */
DC_API void dc_meter(dc_player* p, double* l, double* r);

/* The filter cache folder (UTF-16, NUL-terminated); returns its length, 0 when the cache is disabled
 * (DAPHNE_FILTER_CACHE=off), or DC_ERR_BUFFER. */
DC_API int dc_filter_cache_dir(wchar_t* out, int len);
/* Keeps the filter cache at most limit_mb (0: deletes everything it may), least recently used first,
 * never the filters `settings` (a settings document, UTF-8) use. Returns the bytes left in the cache,
 * or -1 when the settings do not parse. */
DC_API long long dc_filter_cache_prune(const char* settings, int limit_mb);

/* Output devices as JSON: {"wasapi": [{"id","name","default"}], "asio": [{"name","clsid"}]}. */
DC_API int dc_devices(char* json, int len);

#ifdef __cplusplus
}
#endif
#endif /* DAPHNE_CORE_H */
