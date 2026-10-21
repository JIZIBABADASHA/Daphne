// DsdFile on arbitrary bytes: open (DSF or DFF by the magic), read through, seek to the middle and read again.
// A damaged header must fail or open without reading outside the file's data, allocating gigabytes or looping.
#include <algorithm>
#include <string>
#include <vector>

#include "daphne/engine/dsd_file.h"
#include "fuzz_util.h"

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const auto path = fuzz::input_file(".dsd", data, size);
    if (path.empty()) return 0;
    daphne::DsdFile f;
    std::string e;
    if (!f.open(path, &e)) return 0;
    const int ch = f.info().channels;
    std::vector<uint8_t> buf(static_cast<size_t>(4096) * static_cast<size_t>(std::max(ch, 1)));
    // read at most a few MB: a header may claim far more frames than the input has
    for (int i = 0; i < 256; ++i)
        if (f.read(buf.data(), 4096) <= 0) break;
    if (f.info().frames > 0) {
        f.seek(f.info().frames / 2);
        f.read(buf.data(), 4096);
    }
    return 0;
}
