#include <catch_amalgamated.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

#include "daphne/engine/dsd_file.h"
#include "test_util.h"

namespace {

using Fill = std::function<uint8_t(int64_t frame, int channel)>;

uint8_t pattern(int64_t i, int c) { return static_cast<uint8_t>((i * 7 + c * 13 + (i >> 8)) & 0xFF); }

uint8_t rev8(uint8_t b) {
    uint8_t r = 0;
    for (int k = 0; k < 8; ++k) r |= static_cast<uint8_t>(((b >> k) & 1) << (7 - k));
    return r;
}

void put_le(std::vector<uint8_t>& v, uint64_t x, int n) {
    for (int k = 0; k < n; ++k) v.push_back(static_cast<uint8_t>(x >> (8 * k)));
}
void put_be(std::vector<uint8_t>& v, uint64_t x, int n) {
    for (int k = n - 1; k >= 0; --k) v.push_back(static_cast<uint8_t>(x >> (8 * k)));
}
void put_id(std::vector<uint8_t>& v, const char* id) { v.insert(v.end(), id, id + 4); }

std::filesystem::path save(const std::string& name, const std::vector<uint8_t>& bytes) {
    const auto path = test::temp_path(name);
    FILE* fp = daphne::platform::open_file(path, "wb");
    fwrite(bytes.data(), 1, bytes.size(), fp);
    fclose(fp);
    return path;
}

// DSF with `frames` bytes per channel, blocks of 4096 (the last one zero-padded), then an ID3 chunk.
std::filesystem::path write_dsf(const std::string& name, int ch, int64_t frames, int bps, const Fill& fill = pattern) {
    const int block = 4096;
    const int64_t blocks = (frames + block - 1) / block;
    std::vector<uint8_t> data;
    for (int64_t b = 0; b < blocks; ++b)
        for (int c = 0; c < ch; ++c)
            for (int i = 0; i < block; ++i) {
                const int64_t f = b * block + i;
                const uint8_t v = f < frames ? fill(f, c) : 0;
                data.push_back(bps == 1 ? rev8(v) : v);
            }
    const std::vector<uint8_t> id3 = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 10, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55};
    std::vector<uint8_t> f;
    const uint64_t total = 28 + 52 + 12 + data.size() + id3.size();
    put_id(f, "DSD ");
    put_le(f, 28, 8);
    put_le(f, total, 8);
    put_le(f, 28 + 52 + 12 + data.size(), 8);   // metadata pointer
    put_id(f, "fmt ");
    put_le(f, 52, 8);
    put_le(f, 1, 4);                  // version
    put_le(f, 0, 4);                  // format: raw DSD
    put_le(f, ch == 2 ? 2 : 1, 4);    // channel type
    put_le(f, static_cast<uint64_t>(ch), 4);
    put_le(f, 2822400, 4);
    put_le(f, static_cast<uint64_t>(bps), 4);
    put_le(f, static_cast<uint64_t>(frames) * 8, 8);   // samples per channel
    put_le(f, block, 4);
    put_le(f, 0, 4);
    put_id(f, "data");
    put_le(f, 12 + data.size(), 8);
    f.insert(f.end(), data.begin(), data.end());
    f.insert(f.end(), id3.begin(), id3.end());
    return save(name, f);
}

std::filesystem::path write_dff(const std::string& name, const std::vector<const char*>& ids, int64_t frames, const Fill& fill = pattern) {
    const int ch = static_cast<int>(ids.size());
    std::vector<uint8_t> prop, body, f;
    put_id(prop, "SND ");
    put_id(prop, "FS  ");
    put_be(prop, 4, 8);
    put_be(prop, 2822400, 4);
    put_id(prop, "CHNL");
    put_be(prop, 2 + 4 * static_cast<uint64_t>(ch), 8);
    put_be(prop, static_cast<uint64_t>(ch), 2);
    for (const char* id : ids) put_id(prop, id);
    put_id(prop, "CMPR");
    put_be(prop, 4 + 1 + 14 + 1, 8);   // type + pstring (count byte + "not compressed" + pad to even)
    put_id(prop, "DSD ");
    prop.push_back(14);
    const char* nc = "not compressed";
    prop.insert(prop.end(), nc, nc + 14);
    prop.push_back(0);
    put_id(body, "FVER");
    put_be(body, 4, 8);
    put_be(body, 0x01050000, 4);
    put_id(body, "PROP");
    put_be(body, prop.size(), 8);
    body.insert(body.end(), prop.begin(), prop.end());
    put_id(body, "DSD ");
    put_be(body, static_cast<uint64_t>(frames) * ch, 8);
    for (int64_t i = 0; i < frames; ++i)
        for (int c = 0; c < ch; ++c) body.push_back(fill(i, c));
    if ((frames * ch) & 1) body.push_back(0);
    put_id(f, "FRM8");
    put_be(f, 4 + body.size(), 8);
    put_id(f, "DSD ");
    f.insert(f.end(), body.begin(), body.end());
    return save(name, f);
}

bool read_matches(daphne::DsdFile& d, int64_t from, int ch, int chunk) {
    std::vector<uint8_t> buf(static_cast<size_t>(chunk) * ch);
    int64_t i = from;
    for (;;) {
        const int n = d.read(buf.data(), chunk);
        if (n == 0) break;
        for (int k = 0; k < n; ++k, ++i)
            for (int c = 0; c < ch; ++c)
                if (buf[static_cast<size_t>(k) * ch + c] != pattern(i, c)) return false;
    }
    return i == d.info().frames;
}

const std::vector<const char*> kStereo = {"SLFT", "SRGT"};

uint8_t silence(int64_t, int) { return 0x69; }
uint8_t reference(int64_t, int) { return 0xEE; }   // 75% ones = 50% modulation
uint8_t noise(int64_t i, int c) { return static_cast<uint8_t>((static_cast<uint64_t>(i) * 2654435761u + c * 40503u) >> 13); }

}  // namespace

TEST_CASE("DSF reading: 1 and 8 bits per sample, sample count bounds playback", "[dsd]") {
    const int64_t frames = 10000;   // not a multiple of the 4096-byte block
    for (int bps : {1, 8}) {
        daphne::DsdFile d;
        std::string err;
        REQUIRE(d.open(write_dsf("daphne_t.dsf", 2, frames, bps), &err));
        CHECK(d.info().codec == "DSF");
        CHECK(d.info().dsd_rate == 2822400);
        CHECK(d.info().channel_mask == 0x3);
        CHECK(d.info().frames == frames);   // block padding and the ID3 chunk are not played
        CHECK(read_matches(d, 0, 2, 777));
        REQUIRE(d.seek(5000));
        CHECK(read_matches(d, 5000, 2, 4096));
    }
    daphne::DsdFile m;
    REQUIRE(m.open(write_dsf("daphne_m.dsf", 1, 9000, 1), nullptr));
    CHECK(read_matches(m, 0, 1, 1000));
}

TEST_CASE("DFF reading and channel layout", "[dsd]") {
    const int64_t frames = 10000;
    daphne::DsdFile f;
    std::string err;
    REQUIRE(f.open(write_dff("daphne_t.dff", kStereo, frames), &err));
    CHECK(f.info().codec == "DFF");
    CHECK(f.info().channels == 2);
    CHECK(f.info().channel_mask == 0x3);
    CHECK(f.info().frames == frames);
    CHECK(read_matches(f, 0, 2, 999));
    REQUIRE(f.seek(1234));
    CHECK(read_matches(f, 1234, 2, 4096));

    daphne::DsdFile s;
    REQUIRE(s.open(write_dff("daphne_51.dff", {"SLFT", "SRGT", "C   ", "LFE ", "LS  ", "RS  "}, 1000), nullptr));
    CHECK(s.info().channel_mask == 0x3F);
    daphne::DsdFile o;   // not in speaker-mask order: no mask (default order)
    REQUIRE(o.open(write_dff("daphne_odd.dff", {"SLFT", "SRGT", "LS  ", "RS  ", "C   "}, 1000), nullptr));
    CHECK(o.info().channel_mask == 0);
}

TEST_CASE("DSD to PCM: rate, length, silence and the reference level", "[dsd]") {
    {
        auto d = daphne::make_dsd_pcm_decoder();
        REQUIRE(d->open(write_dff("daphne_sil.dff", kStereo, 40000, silence), nullptr));
        CHECK(d->info().rate == 176400);
        CHECK(d->info().frames == 20000);
        CHECK(d->info().dsd_pcm);
        std::vector<double> pcm(20000 * 2);
        REQUIRE(d->read(pcm.data(), 20000) == 20000);
        double peak = 0;
        for (double v : pcm) peak = std::max(peak, std::abs(v));
        CHECK(peak < 1e-5);
    }
    for (int lp : {24, 30, 50}) {
        auto d = daphne::make_dsd_pcm_decoder(0, lp);
        REQUIRE(d->open(write_dsf("daphne_ref.dsf", 2, 40000, 1, reference), nullptr));
        std::vector<double> pcm(20000 * 2);
        REQUIRE(d->read(pcm.data(), 20000) == 20000);
        double worst = 0;
        for (size_t i = 2 * 2000; i < 2 * 18000; ++i) worst = std::max(worst, std::abs(pcm[i] - 1.0));
        INFO("lowpass " << lp << " kHz");
        CHECK(worst < 1e-5);
    }
}

TEST_CASE("DSD to PCM: seek gives the same samples as reading through", "[dsd]") {
    const auto path = write_dsf("daphne_seek.dsf", 2, 60000, 1, noise);
    auto a = daphne::make_dsd_pcm_decoder();
    REQUIRE(a->open(path, nullptr));
    std::vector<double> all(30000 * 2);
    REQUIRE(a->read(all.data(), 30000) == 30000);
    auto b = daphne::make_dsd_pcm_decoder();
    REQUIRE(b->open(path, nullptr));
    REQUIRE(b->seek(12345));
    std::vector<double> part(1000 * 2);
    REQUIRE(b->read(part.data(), 1000) == 1000);
    double diff = 0;
    for (size_t i = 0; i < part.size(); ++i) diff = std::max(diff, std::abs(part[i] - all[12345 * 2 + i]));
    CHECK(diff < 1e-12);
}
