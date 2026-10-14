#include "common.h"

#include <cstdio>

#include "daphne/engine/dsd_output.h"

namespace daphne::playback {

int requested_rate(const Settings& s, int fs_in) {
    if (s.core == CoreMode::Dsd) return dop_rate(dsd_clock(s.modulator.osr));
    return pcm_output_rate(fs_in, s.output.rate);
}

bool same_output(const Settings& cs, int rate, int chain_rate) {
    if (cs.core == CoreMode::Dsd) return modulator_input_rate(rate) == chain_rate;
    for (int r = pcm_output_rate(rate, cs.output.rate); r >= rate && r % rate == 0; r /= 2)
        if (r == chain_rate) return true;
    return false;
}

std::string modulator_key(const Settings& s) {
    if (s.core != CoreMode::Dsd) return "";
    return settings_to_json(s)["dsd_modulator"].dump() + std::to_string(static_cast<int>(s.output.dsd)) + "m" +
           std::to_string(s.output.max_dsd);
}

Settings chain_settings(const Settings& s, int osr) {
    Settings c = s;
    if (osr > 0) c.modulator.osr = osr;
    return c;
}

std::string describe_filter(const Settings& x) {
    static const char* phases[] = {"linear", "intermediate", "minimum", "custom"};
    char b[96];
    std::snprintf(b, sizeof b, "%d taps, %s phase (alpha %.2f)", x.filter.taps, phases[static_cast<int>(x.filter.phase)],
                  x.filter.phase_alpha());
    std::string d = b;
    if (x.filter.apodize) d += ", apodize";
    if (x.filter.double_double) d += ", double-double convolution";
    if (x.core == CoreMode::Dsd)
        d = "DSD modulator order " + std::to_string(x.modulator.order) + ", DSD" + std::to_string(x.modulator.osr) + ", pre-filter " + d;
    return d;
}

std::string join(const std::vector<std::string>& v) {
    std::string s;
    for (const auto& x : v) s += (s.empty() ? "" : " -> ") + x;
    return s.empty() ? "none" : s;
}

}  // namespace daphne::playback
