#include "daphne_core.h"

#include <atomic>
#include <cmath>
#include <cstring>
#include <deque>
#include <memory>
#include <mutex>
#include <string>

#include <nlohmann/json.hpp>

#include "daphne/engine/devices.h"
#include "daphne/engine/filters.h"
#include "daphne/engine/live_source.h"
#include "daphne/engine/player.h"
#include "daphne/engine/sample_format.h"
#include "daphne/engine/settings.h"

using nlohmann::json;

struct dc_player {
    std::mutex op;                       // load / stop / apply
    std::unique_ptr<daphne::Player> player;
    std::mutex m;                        // events, report, state
    std::deque<std::string> events;
    daphne::PlayReport report;
    std::string error;
    int track = 0;
    std::atomic<double> design{1.0};
    bool failed = false;
};

namespace {

void put(char* dst, int len, const std::string& s) {
    if (!dst || len <= 0) return;
    const size_t n = std::min(static_cast<size_t>(len - 1), s.size());
    std::memcpy(dst, s.data(), n);
    dst[n] = 0;
}

int put_json(char* dst, int len, const std::string& s) {
    if (!dst || len <= static_cast<int>(s.size())) return DC_ERR_BUFFER;
    std::memcpy(dst, s.c_str(), s.size() + 1);
    return static_cast<int>(s.size());
}

bool parse(const char* text, daphne::Settings& s, std::string& msg) {
    const auto r = daphne::settings_from_string(text ? text : "{}");
    for (const auto& d : r.diagnostics)
        msg += std::string(d.level == daphne::Diagnostic::Level::Error ? "error " : "warning ") + d.path + ": " + d.message + "\n";
    s = r.settings;
    return r.ok();
}

json report_json(const daphne::PlayReport& r) {
    json src = {{"codec", r.source.codec}, {"rate", r.source.rate}, {"channels", r.source.channels}, {"bits", r.source.bits},
                {"dsd_rate", r.source.dsd_rate}, {"frames", r.source.frames}, {"lossless", r.source.lossless_int}};
    json dev = {{"rate", r.device.rate}, {"channels", r.device.channels}, {"type", daphne::sample_type_name(r.device.type)},
                {"bits", r.device.valid_bits}, {"container", r.device.container_bits}, {"text", daphne::describe(r.device)}};
    return {{"source", src},           {"device", dev},       {"device_name", r.device_name}, {"bit_perfect", r.bit_perfect},
            {"reason", r.not_bit_perfect_reason}, {"filter", r.filter}, {"chain", r.chain},  {"latency_ms", r.latency_ms},
            {"underruns", r.underruns}, {"clipped", r.clipped}};
}

void push_event(dc_player* p, json e) {
    std::lock_guard<std::mutex> l(p->m);
    p->events.push_back(e.dump());
    if (p->events.size() > 256) p->events.pop_front();
}

void stop_player(dc_player* p) {
    if (!p->player) return;
    p->player->stop();
    daphne::PlayReport r;
    std::string e;
    p->player->wait(&r, &e);
    p->player.reset();
}

}  // namespace

extern "C" {

DC_API int dc_abi_version(void) { return 1; }
DC_API const char* dc_version(void) { return "daphne 0.1.0"; }

DC_API dc_player* dc_create(void) {
    try {
        return new dc_player();
    } catch (...) {
        return nullptr;
    }
}

DC_API void dc_destroy(dc_player* p) {
    if (!p) return;
    try {
        std::lock_guard<std::mutex> l(p->op);
        stop_player(p);
    } catch (...) {
    }
    delete p;
}

DC_API int dc_load(dc_player* p, const wchar_t* path, const char* settings_json, double seconds, int play, char* err, int err_len) {
    if (!p || !path) return DC_ERR_ARG;
    try {
        std::lock_guard<std::mutex> l(p->op);
        daphne::Settings s;
        std::string msg;
        if (!parse(settings_json, s, msg)) {
            put(err, err_len, msg);
            return DC_ERR_SETTINGS;
        }
        stop_player(p);
        {
            std::lock_guard<std::mutex> g(p->m);
            p->events.clear();
            p->track = 0;
            p->error.clear();
            p->failed = false;
            p->report = {};
        }
        daphne::OutputChoice out;
        out.api = s.output.api;
        out.device = s.output.device;
        daphne::PlayCallbacks cb;
        cb.on_start = [p](const daphne::PlayReport& r) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->report = r;
            }
            push_event(p, {{"type", "started"}});
        };
        cb.on_track_started = [p](const daphne::PlayReport& r) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->report = r;
                ++p->track;
            }
            push_event(p, {{"type", "track"}});
        };
        cb.on_event = [p](const std::string& e) { push_event(p, {{"type", "info"}, {"text", e}}); };
        cb.on_design = [p](double f) { p->design = f; };
        cb.on_ended = [p]() { push_event(p, {{"type", "ended"}}); };
        cb.on_changed = [p](const daphne::PlayReport& r) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->report = r;
            }
            push_event(p, {{"type", "changed"}});
        };
        cb.on_failed = [p](const std::string& e) {
            {
                std::lock_guard<std::mutex> g(p->m);
                p->failed = true;
                p->error = e;
            }
            push_event(p, {{"type", "failed"}, {"error", e}});
        };
        auto player = std::make_unique<daphne::Player>();
        player->pause(!play);
        std::string e;
        if (!player->start(path, s, out, cb, &e, seconds)) {
            put(err, err_len, e);
            std::lock_guard<std::mutex> g(p->m);
            p->error = e;
            p->failed = true;
            return player->last_failure_was_device() ? DC_ERR_DEVICE : DC_ERR_FILE;
        }
        p->player = std::move(player);
        return DC_OK;
    } catch (const std::exception& ex) {
        put(err, err_len, ex.what());
        return DC_ERR_FILE;
    }
}

DC_API int dc_apply(dc_player* p, const char* settings_json, char* err, int err_len) {
    if (!p) return DC_ERR_ARG;
    try {
        std::lock_guard<std::mutex> l(p->op);
        daphne::Settings s;
        std::string msg;
        if (!parse(settings_json, s, msg)) {
            put(err, err_len, msg);
            return DC_ERR_SETTINGS;
        }
        if (p->player) {
            daphne::OutputChoice out;
            out.api = s.output.api;
            out.device = s.output.device;
            p->player->apply(s, out);
        }
        return DC_OK;
    } catch (...) {
        return DC_ERR_ARG;
    }
}

DC_API void dc_pause(dc_player* p, int paused) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->op);
    if (p->player) p->player->pause(paused != 0);
}

DC_API void dc_seek(dc_player* p, double seconds) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->op);
    if (p->player) p->player->seek(seconds);
}

DC_API void dc_set_next(dc_player* p, const wchar_t* path) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->op);
    if (p->player) p->player->set_next(path ? path : L"");
}

DC_API void dc_set_next_gain(dc_player* p, const wchar_t* path, double headroom_db) {
    if (!p) return;
    std::lock_guard<std::mutex> l(p->op);
    if (p->player)
        p->player->set_next(path ? path : L"", std::isnan(headroom_db) ? std::nullopt : std::optional<double>(headroom_db));
}

DC_API void dc_stop(dc_player* p) {
    if (!p) return;
    try {
        std::lock_guard<std::mutex> l(p->op);
        stop_player(p);
    } catch (...) {
    }
}

DC_API int dc_state(dc_player* p, char* out, int len) {
    if (!p) return DC_ERR_ARG;
    try {
        std::lock_guard<std::mutex> l(p->op);
        json j;
        std::string state = "idle";
        double position = 0, duration = 0;
        if (p->player) {
            if (p->player->ended()) state = "ended";
            else if (p->failed) state = "failed";
            else state = p->player->paused() ? "paused" : "playing";
            position = p->player->position_seconds();
        } else if (p->failed) {
            state = "failed";
        }
        std::lock_guard<std::mutex> g(p->m);
        if (p->report.source.rate > 0 && p->report.source.frames > 0)
            duration = static_cast<double>(p->report.source.frames) / p->report.source.rate;
        j = {{"state", state}, {"position", position}, {"duration", duration}, {"track", p->track},
             {"design", p->design.load()}, {"error", p->error}, {"report", report_json(p->report)}};
        if (const auto ls = daphne::live_stats(); ls.active && p->player)
            j["live"] = {{"fill", ls.fill}, {"read", ls.read}, {"resyncs", ls.resyncs}, {"starves", ls.starves}, {"adjust", ls.adjust}};
        if (p->player) {
            j["asio_rates"] = p->player->asio_rates();
            j["debug"] = json::parse(p->player->debug_json(), nullptr, false);
        }
        return put_json(out, len, j.dump());
    } catch (...) {
        return DC_ERR_ARG;
    }
}

DC_API int dc_poll_event(dc_player* p, char* out, int len) {
    if (!p) return DC_ERR_ARG;
    std::unique_lock<std::mutex> l(p->m);
    if (p->events.empty()) return 0;
    const std::string& e = p->events.front();
    const int r = put_json(out, len, e);
    if (r >= 0) p->events.pop_front();
    return r;
}

DC_API double dc_design(dc_player* p) { return p ? p->design.load() : 1.0; }

DC_API void dc_live_bus(void* base, int cap_frames, int rate) { daphne::set_live_bus(base, cap_frames, rate); }
DC_API void dc_live_rate(int rate) { daphne::set_live_rate(rate); }

DC_API void dc_meter(dc_player* p, double* l, double* r) {
    double a = 0, b = 0;
    if (p) {
        std::lock_guard<std::mutex> g(p->op);
        if (p->player) p->player->meter(a, b);
    }
    if (l) *l = a;
    if (r) *r = b;
}

DC_API int dc_filter_cache_dir(wchar_t* out, int len) {
    try {
        const std::wstring d = daphne::filter_cache_dir();
        if (!out || len <= static_cast<int>(d.size())) return d.empty() ? 0 : DC_ERR_BUFFER;
        std::wmemcpy(out, d.c_str(), d.size() + 1);
        return static_cast<int>(d.size());
    } catch (...) {
        return 0;
    }
}

DC_API long long dc_filter_cache_prune(const char* settings, int limit_mb) {
    try {
        const json j = json::parse(settings ? settings : "{}", nullptr, false);
        if (j.is_discarded()) return -1;
        const auto res = daphne::settings_from_json(j);
        const auto r = daphne::prune_filter_cache(static_cast<uint64_t>(limit_mb < 0 ? 0 : limit_mb) << 20,
                                                 daphne::filter_cache_keys_in_use(res.settings));
        return static_cast<long long>(r.bytes);
    } catch (...) {
        return -1;
    }
}

DC_API int dc_devices(char* out, int len) {
    try {
        json w = json::array(), a = json::array();
        for (const auto& d : daphne::list_wasapi_devices(false)) w.push_back({{"id", d.id}, {"name", d.name}, {"default", d.is_default}});
        for (const auto& d : daphne::list_asio_drivers()) a.push_back({{"name", d.name}, {"clsid", d.clsid}});
        return put_json(out, len, json{{"wasapi", w}, {"asio", a}}.dump());
    } catch (...) {
        return DC_ERR_ARG;
    }
}

}  // extern "C"
