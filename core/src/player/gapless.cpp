// Player::Impl, the next track (core.h): its preparation, the decoder swap, the join through a new chain, its headroom.
#include "core.h"

namespace daphne {

void Player::Impl::kick_prepare() {
    queue.kick([this] {
        TrackQueue::Context c;
        c.dsd = dsdf != nullptr;
        c.cs = chain_settings(s, run_osr);
        c.cf = chain_fmt;
        c.chain_rate = crate;
        c.channels = ch;
        c.cur_rate = decoding_info().rate;
        c.shape = cur ? cur->shape() : DspChain::Shape{};
        c.opener = deps.open_decoder;
        return c;
    });
}

void Player::Impl::cross_switch(std::unique_ptr<Decoder> nd, std::unique_ptr<DspChain> nc, const std::filesystem::path& np) {
    const size_t need = static_cast<size_t>(std::max(cur->max_output_frames(), nc->max_output_frames())) * dch;
    if (dsp.size() < need) dsp.resize(need);
    if (tmp_c.size() < need) tmp_c.resize(need);
    if (tmp_n.size() < need) tmp_n.resize(need);
    const size_t nb = dsdenc ? dsdenc->max_bytes(static_cast<int>(need / dch)) : need / dch * io.block;
    if (io.bytes.size() < nb) io.bytes.resize(nb);

    const int64_t start = tl.abs_out + xf.carry.frames();   // device frame where the joined part begins
    const int64_t a_end = tl.latest_base() + in_total * ratio + cur->content_delay();   // where A's frame K is heard
    std::vector<double> tail;
    auto put = [&](std::vector<double>& v, int n) { v.insert(v.end(), dsp.begin(), dsp.begin() + static_cast<std::ptrdiff_t>(n) * dch); };
    std::fill(pcm.begin(), pcm.end(), 0.0);
    const int64_t zeros = 2 * static_cast<int64_t>(cur->content_delay()) / std::max(1, ratio) + 1;
    for (int64_t z = 0; z < zeros;) {
        const int n = static_cast<int>(std::min<int64_t>(kChunk, zeros - z));
        put(tail, cur->process(pcm.data(), n, dsp.data()));
        z += n;
    }
    put(tail, cur->flush(dsp.data()));

    // B through the new chain until its first sample (new-chain frame dn) is out
    const int dn = nc->content_delay();
    in_total = 0;
    hist.valid_from = 0;
    std::vector<double> head;
    int64_t produced = 0;
    while (produced <= dn) {
        const int got = nd->read(pcm.data(), kChunk);
        if (got <= 0) break;
        hist.store(pcm.data(), got, in_total);
        in_total += got;
        const int o = nc->process(pcm.data(), got, dsp.data());
        put(head, o);
        produced += o;
    }
    // new-chain frame j lands on tail frame j + off; frames before what has been played already are dropped
    const int64_t off = (a_end - start) - dn;
    for (int64_t j = std::max<int64_t>(0, -off); j < produced; ++j) {
        const int64_t t = j + off;
        if (t >= static_cast<int64_t>(tail.size()) / dch) tail.resize(static_cast<size_t>(t + 1) * dch, 0.0);
        for (int c = 0; c < dch; ++c) tail[static_cast<size_t>(t) * dch + c] += head[static_cast<size_t>(j) * dch + c];
    }
    xf.carry.push(tail.data(), static_cast<int>(tail.size() / dch));

    Switch w;
    w.info = nd->info();
    w.path = np;
    w.cross = true;
    w.at_out = a_end - dn;   // B's frame 0 is heard at at_out + new_delay = a_end
    w.new_delay = dn;
    w.old_ratio = ratio;
    w.old_delay = cur->content_delay();
    w.old_base = tl.latest_base();
    rep.clipped += cur->take_clips();   // its tail, now in carry
    w.old_chain = std::move(cur);
    tl.switches.push_back(std::move(w));
    cur = std::move(nc);
    dec = std::move(nd);
    ratio = std::max(1, crate / dec->info().rate);
    throughput.reset();
}

void Player::Impl::apply_headroom(std::optional<double> h) {
    if (!h || tl.switches.empty() || *h == s.headroom_db) return;
    tl.switches.back().old_headroom = s.headroom_db;
    Settings ns = s;
    ns.headroom_db = *h;
    if (cur->update(chain_settings(ns, run_osr))) {
        s = ns;
        return;
    }
    // the chain's structure changes (e.g. bit-perfect -> a quantizer): an ordinary settings change, a little late
    std::lock_guard<std::mutex> l(m);
    if (!pending) pending = std::make_pair(ns, out);
}

void Player::Impl::undo_headroom() {
    for (const auto& w : tl.switches)
        if (w.old_headroom) {
            Settings ns = s;
            ns.headroom_db = *w.old_headroom;
            if (cur && cur->update(chain_settings(ns, run_osr))) s = ns;
            break;
        }
}

void Player::Impl::undo_cross() {
    for (auto& w : tl.switches)
        if (w.cross) {
            cur = std::move(w.old_chain);
            ratio = w.old_ratio;
            break;
        }
}

bool Player::Impl::try_gapless() {
    const auto [np, nh] = queue.get();
    if (np.empty() || is_dsd_path(np) != (dsdf != nullptr)) return false;
    const std::shared_ptr<Prep> p = queue.take(np, cancel);   // opened ahead (kick_prepare)
    std::string e;
    if (dsdf) {
        std::unique_ptr<DsdFile> f = p ? std::move(p->dsdf) : nullptr;
        if (!f) {
            f = std::make_unique<DsdFile>();
            if (!f->open(np, &e)) return false;
        }
        if (f->info().dsd_rate != dsdf->info().dsd_rate || f->info().channels != dsdf->info().channels) return false;
        AudioInfo ai{};
        ai.rate = f->info().dsd_rate / 8;
        ai.channels = f->info().channels;
        ai.bits = 1;
        ai.lossless_int = true;
        ai.frames = f->info().frames;
        ai.channel_mask = f->info().channel_mask;
        ai.codec = f->info().codec;
        ai.dsd_rate = f->info().dsd_rate;
        Switch w;
        w.at_in = tl.abs_out;
        w.info = ai;
        w.path = np;
        tl.switches.push_back(std::move(w));
        dsdf = std::move(f);
    } else {
        std::unique_ptr<Decoder> nd = p ? std::move(p->dec) : nullptr;
        if (!nd) nd = deps.open_decoder(np, &e);
        if (!nd || nd->info().channels != ch) return false;
        const bool fits = cur->fits(chain_settings(s, run_osr), nd->info());
        if (fits) {
            Switch w;
            w.at_in = in_total;
            w.info = nd->info();
            w.path = np;
            tl.switches.push_back(std::move(w));
            dec = std::move(nd);
            apply_headroom(nh);
        } else {
            // another sample rate, or the same rate with a format the current chain does not fit (a 24-bit
            // track after a 16-bit one on a 16-bit device needs the quantizer): gapless only on the same
            // output, joined through the new chain built ahead for the current settings; otherwise the track
            // ends and the host loads the next one
            if (!p || !p->done || !p->chain || xf.fading || building.valid() || p->key != TrackQueue::chain_key(chain_settings(s, run_osr), chain_fmt))
                return false;
            cross_switch(std::move(nd), std::move(p->chain), np);
            apply_headroom(nh);
        }
    }
    queue.consumed(np);
    return true;
}

}  // namespace daphne
