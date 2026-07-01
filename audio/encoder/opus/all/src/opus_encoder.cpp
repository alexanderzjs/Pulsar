// audio/encoder/opus/src/opus_encoder.cpp
// Opus audio encoder using the vendored libopus directly.
// Dependency: audio/encoder/opus/vendor/opus/lib/linux_x86_64/libopus.a  [static]
//             audio/encoder/opus/vendor/opus/include/opus/opus.h          [header]

#include "opus_encoder.h"
#include "opus/opus.h"

#include <cstring>
#include <iostream>
#include <memory>
#include <vector>

namespace pulsar::audio::encoder::opus {

struct OpusEncoder::Impl {
    ::OpusEncoder* enc = nullptr;
    int sample_rate    = 48000;
    int channels       = 2;
    int frame_size     = 960;   // 20 ms @ 48 kHz
};

OpusEncoder::OpusEncoder() : impl_(std::make_unique<Impl>()) {
    open(48000, 2);
}
OpusEncoder::~OpusEncoder() { close(); }

bool OpusEncoder::open(int sample_rate, int channels) {
    close();
    impl_->sample_rate = sample_rate;
    impl_->channels    = channels;
    impl_->frame_size  = sample_rate / 50;  // 20 ms

    int err = OPUS_OK;
    impl_->enc = opus_encoder_create(sample_rate, channels,
                                     OPUS_APPLICATION_AUDIO, &err);
    if (err != OPUS_OK || !impl_->enc) {
        std::cerr << "[opus] opus_encoder_create failed: "
                  << opus_strerror(err) << "\n";
        return false;
    }
    // Low-delay streaming preset
    opus_encoder_ctl(impl_->enc, OPUS_SET_BITRATE(128000));
    opus_encoder_ctl(impl_->enc, OPUS_SET_COMPLEXITY(5));
    opus_encoder_ctl(impl_->enc, OPUS_SET_SIGNAL(OPUS_SIGNAL_MUSIC));
    return true;
}

void OpusEncoder::close() {
    if (impl_ && impl_->enc) {
        opus_encoder_destroy(impl_->enc);
        impl_->enc = nullptr;
    }
}

void OpusEncoder::submit_frame(pulsar::core::AudioFrame frame) {
    if (!callback_ || !impl_->enc || !frame.data) return;

    // Re-open if sample rate or channel count changed.
    if (frame.sample_rate != impl_->sample_rate ||
        frame.channels    != impl_->channels) {
        if (!open(frame.sample_rate, frame.channels)) return;
    }

    const int frame_size = impl_->frame_size;
    const int channels   = impl_->channels;
    const auto* pcm      = reinterpret_cast<const opus_int16*>(frame.data.get());

    // Encode in frame_size-sample blocks.
    const int input_samples = frame.samples > 0 ? frame.samples : frame_size;
    const int blocks        = std::max(1, input_samples / frame_size);

    constexpr int kMaxPacket = 4000;
    std::vector<uint8_t> out(static_cast<size_t>(kMaxPacket));

    for (int b = 0; b < blocks; ++b) {
        const opus_int16* block_pcm = pcm + b * frame_size * channels;
        const opus_int32  written   = opus_encode(impl_->enc, block_pcm, frame_size,
                                                   out.data(), kMaxPacket);
        if (written <= 0) continue;

        struct OpusPacketBuffer final : public pulsar::core::PacketBuffer {
            std::vector<uint8_t> d_;
            OpusPacketBuffer(const uint8_t* s, size_t n) : d_(s, s + n) {}
            const uint8_t* data() const override { return d_.data(); }
            size_t         size() const override { return d_.size(); }
        };

        pulsar::core::AudioPacket pkt;
        pkt.data   = std::make_shared<uint8_t[]>(static_cast<size_t>(written));
        std::memcpy(pkt.data.get(), out.data(), static_cast<size_t>(written));
        pkt.size   = static_cast<size_t>(written);
        pkt.pts_us = frame.pts_us
                   + static_cast<int64_t>(b) * frame_size * 1'000'000
                     / impl_->sample_rate;
        pkt.codec  = pulsar::core::AudioCodec::OPUS;
        callback_(std::move(pkt));
    }
}

void OpusEncoder::set_encoded_callback(
    std::function<void(pulsar::core::AudioPacket)> cb) {
    callback_ = std::move(cb);
}

void OpusEncoder::set_bitrate_kbps(int kbps) {
    if (!impl_ || !impl_->enc || kbps <= 0) return;
    opus_encoder_ctl(impl_->enc, OPUS_SET_BITRATE(kbps * 1000));
}

pulsar::core::AdapterCapabilities OpusEncoder::capabilities() const {
    return {};
}

} // namespace pulsar::audio::encoder::opus
