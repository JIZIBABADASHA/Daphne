#include <catch_amalgamated.hpp>

#include <atomic>
#include <cmath>
#include <thread>
#include <vector>

#include "daphne/dsp/crossfeed.h"
#include "daphne/dsp/eq.h"
#include "daphne/engine/stages.h"

using namespace daphne::dsp;

namespace {

double db(double x) { return 20.0 * std::log10(x); }

std::unique_ptr<PeqParams> params(std::vector<Biquad> s, double preamp = 1.0, bool on = true) {
    auto p = std::make_unique<PeqParams>();
    p->sections = std::move(s);
    p->preamp = preamp;
    p->enabled = on;
    return p;
}

// direct form I, sample by sample: the reference for the block cascade
std::vector<double> reference(const std::vector<Biquad>& secs, double pre, const std::vector<double>& x, int ch) {
    std::vector<double> y(x.size());
    std::vector<double> st(secs.size() * ch * 4, 0.0);
    for (size_t i = 0; i < x.size() / ch; ++i)
        for (int c = 0; c < ch; ++c) {
            double v = x[i * ch + c] * pre;
            for (size_t s = 0; s < secs.size(); ++s) {
                double* z = &st[(s * ch + c) * 4];
                const Biquad& q = secs[s];
                const double o = q.b0 * v + q.b1 * z[0] + q.b2 * z[1] - q.a1 * z[2] - q.a2 * z[3];
                z[1] = z[0], z[0] = v, z[3] = z[2], z[2] = o;
                v = o;
            }
            y[i * ch + c] = v;
        }
    return y;
}

}  // namespace

TEST_CASE("EQ design: peaking, shelves, pass filters", "[eq]") {
    const double fs = 44100;
    // peaking: the gain at f0, 0 dB far away; also above 0.2 fs (matched design, no cramping)
    for (double f0 : {1000.0, 6000.0, 12000.0, 16000.0}) {
        const auto q = peq_peaking(fs, f0, 2.0, 6.0);
        INFO("f0 " << f0);
        CHECK(db(peq_magnitude({q}, f0, fs)) == Catch::Approx(6.0).margin(0.05));
        CHECK(std::abs(db(peq_magnitude({q}, 20.0, fs))) < 0.01);
    }
    CHECK(peq_peaking(fs, 1000.0, 1.0, 0.00001).b0 == 1.0);   // ~0 dB: pass-through
    // shelves: +6 dB below / above the corner, 0 dB on the other side, half the gain at the corner
    const auto ls = peq_shelf(false, fs, 200.0, 1.0, 6.0), hs = peq_shelf(true, fs, 5000.0, 1.0, 6.0);
    CHECK(db(peq_magnitude({ls}, 10.0, fs)) == Catch::Approx(6.0).margin(0.02));
    CHECK(std::abs(db(peq_magnitude({ls}, 20000.0, fs))) < 0.02);
    CHECK(db(peq_magnitude({ls}, 200.0, fs)) == Catch::Approx(3.0).margin(0.05));
    CHECK(db(peq_magnitude({hs}, 22000.0, fs)) == Catch::Approx(6.0).margin(0.05));
    CHECK(std::abs(db(peq_magnitude({hs}, 20.0, fs))) < 0.02);
    CHECK(db(peq_magnitude({hs}, 5000.0, fs)) == Catch::Approx(3.0).margin(0.05));
    // 24 dB pass filters: two sections, -6 dB at f
    std::vector<PeqBand> bands = {{true, PeqType::HighPass24, 80.0, 0.0, 0.7071}, {true, PeqType::LowPass24, 15000.0, 0.0, 0.7071}};
    const auto s = design_peq(bands, fs);
    REQUIRE(s.size() == 4);
    CHECK(db(peq_magnitude({s[0], s[1]}, 80.0, fs)) == Catch::Approx(-6.02).margin(0.02));
    CHECK(db(peq_magnitude({s[2], s[3]}, 15000.0, fs)) == Catch::Approx(-6.02).margin(0.02));
    // band-pass peaks at 0 dB; all-pass is flat
    bands = {{true, PeqType::BandPass, 1000.0, 0.0, 2.0}, {true, PeqType::AllPass, 3000.0, 0.0, 1.0}};
    const auto bp = design_peq(bands, fs);
    CHECK(std::abs(db(peq_magnitude({bp[0]}, 1000.0, fs))) < 1e-9);
    for (double f : {100.0, 3000.0, 15000.0}) CHECK(std::abs(db(peq_magnitude({bp[1]}, f, fs))) < 1e-9);
    // disabled bands and nothing else are left out (no extra sections)
    bands = {{false, PeqType::Peaking, 8000.0, 6.0, 1.0}, {true, PeqType::Peaking, 9000.0, 3.0, 1.0}};
    CHECK(design_peq(bands, fs).size() == 1);
}

TEST_CASE("EQ automatic preamp: response peak + overshoot + 0.2 dB, manual only lowers", "[eq]") {
    const double fs = 48000;
    std::vector<PeqBand> bands = {{true, PeqType::Peaking, 1000.0, 6.0, 0.5}};   // Q <= 0.7071: no overshoot term
    auto s = design_peq(bands, fs);
    CHECK(peq_auto_preamp_db(bands, s, fs) == Catch::Approx(-6.2).margin(0.01));
    bands = {{true, PeqType::Peaking, 1000.0, -6.0, 2.0}};   // cuts only: 0 dB
    s = design_peq(bands, fs);
    CHECK(peq_auto_preamp_db(bands, s, fs) == 0.0);
    bands = {{true, PeqType::Peaking, 1000.0, 6.0, 4.0}};    // high Q boost: the overshoot adds headroom
    s = design_peq(bands, fs);
    CHECK(peq_auto_preamp_db(bands, s, fs) < -6.2);

    daphne::EqSettings e;
    e.enabled = true;
    e.bands = {{daphne::EqType::PK, 1000.0, 0.5, 6.0, true}};
    auto p = daphne::make_peq_params(e, fs);
    CHECK(db(p->preamp) == Catch::Approx(-6.2).margin(0.01));
    e.preamp_db = -10.0;
    CHECK(db(daphne::make_peq_params(e, fs)->preamp) == Catch::Approx(-10.0));
    e.preamp_db = -2.0;   // above the automatic one: the automatic one stays
    CHECK(db(daphne::make_peq_params(e, fs)->preamp) == Catch::Approx(-6.2).margin(0.01));
    e.bands[0].on = false;
    CHECK_FALSE(daphne::make_peq_params(e, fs)->enabled);
}

TEST_CASE("Peq: the block cascade equals direct form I sample by sample", "[eq]") {
    const double fs = 44100;
    std::vector<PeqBand> bands = {{true, PeqType::LowShelf, 100.0, 4.0, 0.8}, {true, PeqType::Peaking, 2500.0, -3.0, 1.4},
                                  {true, PeqType::HighShelf, 9000.0, 2.0, 1.0}, {true, PeqType::HighPass24, 25.0, 0.0, 0.7}};
    const auto secs = design_peq(bands, fs);
    for (int ch : {1, 2, 6}) {
        Peq q;
        q.set_params(params(secs, 0.7));
        q.prepare({44100, ch, 1000});
        std::vector<double> x(5000 * ch);
        for (size_t i = 0; i < x.size(); ++i) x[i] = std::sin(0.0137 * double(i) * (1 + i % ch)) * 0.5;
        std::vector<double> y(x.size());
        for (int off = 0; off < 5000; off += 1000) q.process(x.data() + off * ch, 1000, y.data() + off * ch);
        const auto r = reference(secs, 0.7, x, ch);
        double d = 0;
        for (size_t i = 0; i < y.size(); ++i) d = std::max(d, std::abs(y[i] - r[i]));
        INFO("channels " << ch);
        CHECK(d < 1e-14);
        // in place gives the same
        Peq qi;
        qi.set_params(params(secs, 0.7));
        qi.prepare({44100, ch, 1000});
        std::vector<double> z = x;
        for (int off = 0; off < 5000; off += 1000) qi.process(z.data() + off * ch, 1000, z.data() + off * ch);
        CHECK(z == y);
    }
}

TEST_CASE("Peq: changes glide or crossfade without a step; off is identity", "[eq]") {
    Peq q;
    q.prepare({48000, 1, 480});
    CHECK(q.is_identity());
    std::vector<double> x(48000, 0.5), y(48000);
    // turning on (+6 dB low shelf, DC input): a 50 ms raised-cosine crossfade from the dry signal
    q.set_params(params({peq_shelf(false, 48000, 100.0, 1.0, 6.0)}));
    CHECK_FALSE(q.is_identity());
    for (int off = 0; off < 48000; off += 480) q.process(x.data() + off, 480, y.data() + off);
    double jump = 0;
    for (int i = 1; i < 48000; ++i) jump = std::max(jump, std::abs(y[i] - y[i - 1]));
    CHECK(jump < 0.002);
    CHECK(y.back() == Catch::Approx(0.5 * std::pow(10.0, 6.0 / 20.0)).epsilon(1e-3));
    // same section count, new gain: glides (no step), settles on the new value
    q.set_params(params({peq_shelf(false, 48000, 100.0, 1.0, -6.0)}));
    for (int off = 0; off < 48000; off += 480) q.process(x.data() + off, 480, y.data() + off);
    jump = 0;
    for (int i = 1; i < 48000; ++i) jump = std::max(jump, std::abs(y[i] - y[i - 1]));
    CHECK(jump < 0.005);
    CHECK(y.back() == Catch::Approx(0.5 * std::pow(10.0, -6.0 / 20.0)).epsilon(1e-3));
    // off again: crossfade back, then a bit-exact pass-through
    q.set_params(params({}, 1.0, false));
    for (int off = 0; off < 48000; off += 480) q.process(x.data() + off, 480, y.data() + off);
    CHECK(q.is_identity());
    q.process(x.data(), 480, y.data());
    for (int i = 0; i < 480; ++i) REQUIRE(y[i] == x[i]);
}

TEST_CASE("Peq: an unstable section is cleared and outputs silence", "[eq]") {
    Peq q;
    q.prepare({48000, 2, 256});
    q.set_params(params({Biquad{1.0, 0.0, 0.0, 0.0, -1.5}}));   // pole outside the unit circle
    q.reset();
    std::vector<double> x(512, 0.5), y(512);
    bool silent = false;
    for (int k = 0; k < 200 && !silent; ++k) {
        q.process(x.data(), 256, y.data());
        silent = std::all_of(y.begin(), y.end(), [](double v) { return v == 0.0; });
        for (double v : y) REQUIRE(std::isfinite(v));
    }
    CHECK(silent);
}

TEST_CASE("Peq: parameters posted from another thread while processing", "[eq]") {
    Peq q;
    q.prepare({48000, 2, 256});
    std::atomic<bool> stop{false};
    std::thread t([&] {
        for (int i = 0; !stop; ++i) {
            const int n = 1 + i % 3;
            std::vector<Biquad> s;
            for (int k = 0; k < n; ++k) s.push_back(peq_peaking(48000, 500.0 * (k + 1), 1.0, (i % 7) - 3.0));
            q.set_params(params(s, 0.5, i % 5 != 0));
        }
    });
    std::vector<double> x(512, 0.25), y(512);
    for (int k = 0; k < 20000; ++k) {
        q.process(x.data(), 256, y.data());
        for (double v : y) REQUIRE(std::isfinite(v));
    }
    stop = true;
    t.join();
}

TEST_CASE("Crossfeed: one-pole mix, centre level kept, off is identity", "[crossfeed]") {
    Crossfeed c;
    c.prepare({44100, 2, 44100});
    CHECK(c.is_identity());
    c.set_params(true, 0.3, 700.0);
    CHECK_FALSE(c.is_identity());
    // a hard-left DC signal: left settles at 0.7, right at 0.3 (the low-passed left)
    std::vector<double> x(2 * 44100), y(2 * 44100);
    for (int i = 0; i < 44100; ++i) x[2 * i] = 1.0;
    c.process(x.data(), 44100, y.data());
    CHECK(y[2 * 44099] == Catch::Approx(0.7));
    CHECK(y[2 * 44099 + 1] == Catch::Approx(0.3).epsilon(1e-9));
    const double a = (1.0 / 44100) / (1.0 / (700.0 * 6.283185307179586) + 1.0 / 44100);
    CHECK(y[1] == Catch::Approx(0.3 * a));   // the cross path starts low (low-pass)
    // centre (L = R) DC keeps its level
    c.reset();
    std::fill(x.begin(), x.end(), 0.5);
    c.process(x.data(), 44100, y.data());
    CHECK(y[2 * 44099] == Catch::Approx(0.5));
    // mono passes through
    Crossfeed m;
    m.set_params(true, 0.5, 700.0);
    m.prepare({44100, 1, 16});
    CHECK(m.is_identity());
}
