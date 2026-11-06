// Wake word spike (Kconfig HG_WAKEWORD_SPIKE). See port_wake_spike.cpp.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hg/hal.hpp"

namespace hgp::wake_spike {
// Console `wake [seconds] [90|95]`, `wake on [90|95]` (until `wake off`), `wake off`:
// keeps the microphone open and listens. Returns at once; detections and the
// summary are printed as `@wake` lines.
std::string start(hg::AudioIn& mic, std::string_view args);
// After every App::tick(): `busy` is App::in_turn(). Returns true once per
// detection, when the app should start a hands-free recording. Pauses detection
// while busy (no echo cancellation on this board) and reopens the microphone,
// which the app closes after each recording, once the turn is over.
bool tick(bool busy);
// Every microphone chunk (mono int16 at `rate` Hz) while the spike listens.
void feed(const int16_t* samples, size_t count, uint32_t rate);
bool active();
}  // namespace hgp::wake_spike
