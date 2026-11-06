// Wake word spike (Kconfig HG_WAKEWORD_SPIKE): does ESP-SR WakeNet10 hear
// "Hey Hermes" through this board's microphone, and what does it cost?
//
// `wake [seconds] [90|95]` on the console opens the microphone (the app ignores
// samples while it is not recording) and every chunk the app task receives is
// also handed to feed(): resampled 24 -> 16 kHz and run through WakeNet on the
// app task, timed. Each detection prints `@wake detected`; when the time is up,
// `@wake summary` gives detect time, CPU share and the memory WakeNet took.
#include "sdkconfig.h"
#if CONFIG_HG_WAKEWORD_SPIKE

#include "port_wake_spike.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"

namespace hgp::wake_spike {
namespace {

hg::AudioIn* g_mic = nullptr;
const esp_wn_iface_t* g_wn = nullptr;
model_iface_data_t* g_model = nullptr;
char* g_name = nullptr;
int g_chunk = 0;
det_mode_t g_mode = DET_MODE_90;
std::vector<int16_t> g_buf;  // 16 kHz samples waiting for a full chunk
float g_phase = 0;           // resampler position, in input samples
int16_t g_last = 0;          // previous input sample, for interpolation

int64_t g_start = 0, g_end = 0, g_detect_us = 0, g_detect_max = 0;
uint32_t g_chunks = 0, g_hits = 0;
size_t g_init_internal = 0, g_init_psram = 0;
bool g_active = false;
bool g_paused = false;     // a turn is running: ignore the microphone
bool g_pending = false;    // detected: start a recording on the next tick
int64_t g_resume_at = 0;   // after a turn, listen again from here

std::string init(det_mode_t mode) {
  if (g_model && mode == g_mode) return "";
  if (g_model) {
    g_wn->destroy(g_model);
    g_model = nullptr;
  }
  const size_t in0 = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  const size_t ps0 = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  static srmodel_list_t* models = esp_srmodel_init("model");
  if (!models) return "no ESP-SR models: flash srmodels.bin to the `model` partition";
  if (!g_name) g_name = esp_srmodel_filter(models, ESP_WN_PREFIX, nullptr);
  if (!g_name) return "no WakeNet model in the `model` partition";
  g_wn = esp_wn_handle_from_name(g_name);
  if (!g_wn) return std::string("no WakeNet handle for ") + g_name;
  g_model = g_wn->create(g_name, mode);
  if (!g_model) return "WakeNet create failed";
  g_mode = mode;
  g_chunk = g_wn->get_samp_chunksize(g_model);
  g_init_internal = in0 - heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
  g_init_psram = ps0 - heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
  std::printf("@wake model=%s rate=%d chunk=%d mode=%d internal_kb=%d psram_kb=%d\n", g_name,
              g_wn->get_samp_rate(g_model), g_chunk, static_cast<int>(mode), static_cast<int>(g_init_internal / 1024),
              static_cast<int>(g_init_psram / 1024));
  return "";
}

void finish() {
  g_active = false;
  if (g_mic) g_mic->stop();
  const double secs = (esp_timer_get_time() - g_start) / 1e6;
  std::printf(
      "@wake summary seconds=%.1f detections=%u chunks=%u detect_avg_ms=%.2f detect_max_ms=%.2f cpu_pct=%.1f "
      "free_internal_kb=%d\n",
      secs, static_cast<unsigned>(g_hits), static_cast<unsigned>(g_chunks),
      g_chunks ? g_detect_us / 1000.0 / g_chunks : 0.0, g_detect_max / 1000.0,
      secs > 0 ? 100.0 * g_detect_us / 1e6 / secs : 0.0,
      static_cast<int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024));
}

}  // namespace

bool active() { return g_active; }

std::string start(hg::AudioIn& mic, std::string_view args) {
  int seconds = 30, mode = 90;
  const std::string a(args);
  if (a == "wake off") {
    if (!g_active) return "@ok wake already off";
    finish();
    return "@ok wake off";
  }
  bool forever = false;
  if (a.rfind("wake on", 0) == 0) {
    forever = true;
    std::sscanf(a.c_str(), "wake on %d", &mode);
  } else {
    std::sscanf(a.c_str(), "wake %d %d", &seconds, &mode);
  }
  seconds = std::max(1, std::min(600, seconds));
  const std::string err = init(mode == 95 ? DET_MODE_95 : DET_MODE_90);
  if (!err.empty()) return "@error wake: " + err;
  g_mic = &mic;
  if (!mic.start(24000)) return "@error wake: the microphone did not start";
  g_buf.clear();
  g_phase = 0;
  g_detect_us = g_detect_max = 0;
  g_chunks = g_hits = 0;
  g_start = esp_timer_get_time();
  g_end = forever ? INT64_MAX : g_start + static_cast<int64_t>(seconds) * 1000000;
  g_active = true;
  g_paused = g_pending = false;
  return "@ok wake listening " + (forever ? std::string("until `wake off`") : std::to_string(seconds) + " s") +
         " for: " + g_name;
}

void feed(const int16_t* in, size_t count, uint32_t rate) {
  if (!g_active || g_paused || g_pending || esp_timer_get_time() < g_resume_at) return;
  // Linear-interpolating resampler from `rate` to 16 kHz (24 kHz in practice:
  // two outputs per three inputs). Speech sits under 4 kHz, so skipping a proper
  // low-pass costs little for a spike.
  const float step = static_cast<float>(rate) / 16000.0f;
  for (size_t i = 0; i < count; ++i) {
    const int16_t cur = in[i];
    while (g_phase < 1.0f) {
      g_buf.push_back(static_cast<int16_t>(g_last + (cur - g_last) * g_phase));
      g_phase += step;
    }
    g_phase -= 1.0f;
    g_last = cur;
  }
  size_t used = 0;
  while (g_buf.size() - used >= static_cast<size_t>(g_chunk)) {
    const int64_t t0 = esp_timer_get_time();
    const wakenet_state_t r = g_wn->detect(g_model, g_buf.data() + used);
    const int64_t dt = esp_timer_get_time() - t0;
    g_detect_us += dt;
    g_detect_max = std::max(g_detect_max, dt);
    ++g_chunks;
    used += g_chunk;
    if (r == WAKENET_DETECTED) {
      ++g_hits;
      std::printf("@wake detected t=%.2fs\n", (esp_timer_get_time() - g_start) / 1e6);
      g_pending = true;
      g_buf.clear();
      return;
    }
  }
  if (used) g_buf.erase(g_buf.begin(), g_buf.begin() + used);
  if (esp_timer_get_time() >= g_end) finish();
}

bool tick(bool busy) {
  if (!g_active) return false;
  if (esp_timer_get_time() >= g_end) {
    finish();
    return false;
  }
  if (busy) {
    g_paused = true;
    g_pending = false;
    return false;
  }
  if (g_paused) {  // the turn is over: the app closed the microphone after recording
    g_paused = false;
    g_buf.clear();
    g_resume_at = esp_timer_get_time() + 1000 * 1000;  // skip the tail of the reply
    if (g_mic) g_mic->start(24000);
    return false;
  }
  if (g_pending) {
    g_pending = false;
    return true;
  }
  return false;
}

}  // namespace hgp::wake_spike

#endif  // CONFIG_HG_WAKEWORD_SPIKE
