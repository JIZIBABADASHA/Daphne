// The outputs of the Windows build: WASAPI exclusive / shared and ASIO (N24).
#include "daphne/engine/asio_output.h"
#include "daphne/engine/player.h"
#include "daphne/engine/wasapi_output.h"

namespace daphne {

OutputOpener platform_output_opener() {
    return [](const OutputRequest& r, std::string* error) -> std::unique_ptr<AudioOutput> {
        if (r.out.api == OutputApi::Asio) {
            auto a = std::make_unique<AsioOutput>();
            if (!a->open({r.out.device, r.dsd}, r.rate, r.channels, error)) return nullptr;
            return a;
        }
        auto w = std::make_unique<WasapiOutput>();
        if (!w->open({r.out.device, r.out.api != OutputApi::WasapiShared, r.buffer_ms}, r.rate, r.channels, error)) return nullptr;
        return w;
    };
}

}  // namespace daphne
