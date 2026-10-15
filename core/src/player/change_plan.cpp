#include "change_plan.h"

#include "common.h"
#include "daphne/engine/dsd_file.h"

namespace daphne::playback {

bool decoder_changes(const Playing& now, const Settings& ns) {
    return is_dsd_path(now.path) && ns.output.dsd_pcm_lowpass != now.s.output.dsd_pcm_lowpass;
}

bool buffer_changes(const Playing& now, const Settings& ns) {
    return now.caps.takes_buffer_ms && ns.output.buffer_ms != now.s.output.buffer_ms;
}

bool needs_restart(const Playing& now, const Settings& ns, const OutputChoice& no) {
    return !(no == now.out) || ns.core != now.s.core || requested_rate(ns, now.source_rate) != now.want ||
           modulator_key(ns) != modulator_key(now.s) || decoder_changes(now, ns) || buffer_changes(now, ns);
}

bool dsd_direct_reopens(const Playing& now, const Settings& ns, const OutputChoice& no) {
    return !(no == now.out) || ns.output.dsd != now.s.output.dsd || buffer_changes(now, ns);
}

}  // namespace daphne::playback
