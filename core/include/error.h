#pragma once

#include <string>

namespace pulsar::core {

enum class StreamError {
    None,
    ConfigInvalid,
    AuthFailed,
    TransportUnavailable,
    CaptureDeviceLost,
    EncoderFailed,
    PipelineAborted,
    InputPermissionDenied,
    Unknown,
};

inline const char* to_string(StreamError e) {
    switch (e) {
    case StreamError::None:                  return "none";
    case StreamError::ConfigInvalid:         return "config_invalid";
    case StreamError::AuthFailed:            return "auth_failed";
    case StreamError::TransportUnavailable:  return "transport_unavailable";
    case StreamError::CaptureDeviceLost:     return "capture_device_lost";
    case StreamError::EncoderFailed:         return "encoder_failed";
    case StreamError::PipelineAborted:       return "pipeline_aborted";
    case StreamError::InputPermissionDenied: return "input_permission_denied";
    case StreamError::Unknown:               return "unknown";
    }
    return "unknown";
}

} // namespace pulsar::core
