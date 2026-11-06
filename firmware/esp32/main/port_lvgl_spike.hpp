// LVGL spike (Kconfig HG_LVGL_SPIKE). See port_lvgl_spike.cpp.
#pragma once
#include <string>
#include <string_view>

#include "hg/hal.hpp"

namespace hgp::lvgl_spike {
// Runs the benchmark on the calling (app) task and returns `@lvgl` result lines.
// `args` is the whole console line: "lvgl" or "lvgl <seconds per scene>".
std::string run(hg::Display& display, std::string_view args);
}  // namespace hgp::lvgl_spike
