#include "daphne/engine/stages.h"

#include <algorithm>
#include <cmath>

namespace daphne {

std::vector<dsp::PeqBand> peq_bands(const EqSettings& s) {
    std::vector<dsp::PeqBand> out;
    out.reserve(s.bands.size());
    for (const EqBand& b : s.bands)
        out.push_back({b.on, static_cast<dsp::PeqType>(static_cast<int>(b.type)), b.f, b.gain_db, b.q});
    return out;
}

std::unique_ptr<dsp::PeqParams> make_peq_params(const EqSettings& s, double fs) {
    auto p = std::make_unique<dsp::PeqParams>();
    const auto bands = peq_bands(s);
    p->sections = dsp::design_peq(bands, fs);
    if (p->sections.size() > static_cast<size_t>(dsp::Peq::kMaxSections)) p->sections.resize(dsp::Peq::kMaxSections);
    double pre = dsp::peq_auto_preamp_db(bands, p->sections, fs);
    if (s.preamp_db) pre = std::min(*s.preamp_db, pre);
    p->preamp = std::pow(10.0, pre / 20.0);
    p->enabled = s.enabled && !p->sections.empty();
    return p;
}

void configure_crossfeed(dsp::Crossfeed& c, const CrossfeedSettings& s) { c.set_params(s.enabled, s.amount, s.fc); }

}  // namespace daphne
