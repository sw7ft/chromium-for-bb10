// Copyright 2025 SW7FT. All rights reserved.
// Touch gesture tuning for QNX / BB10 phones (Passport-class 720px panels).

#include "ui/events/gesture_detection/gesture_configuration.h"

#include "base/command_line.h"
#include "base/memory/singleton.h"
#include "ui/events/event_switches.h"

namespace ui {
namespace {

class GestureConfigurationQnx : public GestureConfiguration {
 public:
  GestureConfigurationQnx(const GestureConfigurationQnx&) = delete;
  GestureConfigurationQnx& operator=(const GestureConfigurationQnx&) = delete;
  ~GestureConfigurationQnx() override = default;

  static GestureConfigurationQnx* GetInstance() {
    return base::Singleton<GestureConfigurationQnx>::get();
  }

 private:
  GestureConfigurationQnx() : GestureConfiguration() {
    // Aura defaults (min_scaling_span=125px) delay pinch until fingers are very
    // far apart on a ~720px panel — feels like "lag" even at 60fps presents.
    set_gesture_begin_end_types_enabled(true);
    set_max_touch_move_in_pixels_for_click(10.f);
    set_span_slop(16.f);
    set_min_scaling_span_in_pixels(48.f);
    set_min_distance_for_pinch_scroll_in_pixels(8.f);
    set_min_pinch_update_span_delta(
        base::CommandLine::ForCurrentProcess()->HasSwitch(
            switches::kCompensateForUnstablePinchZoom)
            ? 3.f
            : 0.f);
    set_two_finger_tap_enabled(true);
    set_swipe_enabled(true);
  }

  friend struct base::DefaultSingletonTraits<GestureConfigurationQnx>;
};

}  // namespace

GestureConfiguration* GestureConfiguration::GetPlatformSpecificInstance() {
  return GestureConfigurationQnx::GetInstance();
}

}  // namespace ui
