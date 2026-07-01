#pragma once

#include "capabilities.h"
#include "frame.h"

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace pulsar::core {

struct CursorState {
    int  x = 0, y = 0;
    bool visible = true;
    std::vector<uint8_t> image_rgba;
    int  image_width  = 0;
    int  image_height = 0;
    int  hotspot_x    = 0;
    int  hotspot_y    = 0;
};

struct DisplayInfo {
    int         index        = 0;
    std::string name;
    int         width        = 0;
    int         height       = 0;
    int         refresh_rate = 60;
    bool        is_primary   = false;
    bool        hdr_supported = false;
};

enum class CaptureEvent { DeviceLost, FormatChanged, ResolutionChanged, DisplayChanged };

class ICaptureSource : public ICapabilityProvider {
public:
    virtual ~ICaptureSource() = default;
    virtual std::optional<RawFrame>    next_frame()   = 0;
    virtual std::optional<CursorState> next_cursor()  = 0;
    virtual std::vector<DisplayInfo>   enumerate_displays() const = 0;
    virtual void select_display(int index) = 0;
    virtual int  display_refresh_rate() const = 0;
    virtual bool supports_window_capture() const { return false; }
    virtual bool select_window(uint64_t) { return false; }
    virtual void set_event_callback(std::function<void(CaptureEvent)> cb) = 0;
};

} // namespace pulsar::core
