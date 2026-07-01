#pragma once

#include "frame.h"
#include <string>
#include <vector>

namespace pulsar::core {

// Codecs the client can decode.
struct ClientCapabilities {
    std::vector<CodecType>   video_codecs;   // e.g. H264, H265, AV1
    std::vector<int>         max_resolutions; // max pixels (width * height)
    int                      max_fps         = 60;
    bool                     supports_hdr    = false;
};

// Parameters agreed upon after capability exchange.
struct NegotiatedParams {
    int         bitrate_kbps   = 8000;
    int         fps            = 60;
    int         width          = 1920;
    int         height         = 1080;
    CodecType   video_codec    = CodecType::H264;
    bool        hdr_enabled    = false;
};

class ICapabilityNegotiator {
public:
    virtual ~ICapabilityNegotiator() = default;
    virtual NegotiatedParams negotiate(const ClientCapabilities& client) const = 0;
};

} // namespace pulsar::core
