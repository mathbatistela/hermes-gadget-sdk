// LVGL spike (Kconfig HG_LVGL_SPIKE): can LVGL 9 render on this port, and how fast?
//
// The console's `lvgl [seconds]` takes the screen on the app task (so nothing else
// draws meanwhile), runs three scenes for `seconds` each (default 4), and prints one
// `@lvgl` line per scene plus the memory LVGL took. LVGL renders into two small
// buffers in internal RAM; the flush callback copies each area into the port's own
// framebuffer and pushes it with the port's own Display::flush, the path the device
// UI uses. The caller then asks the display for a full repaint, so the device UI
// comes back.
#include "sdkconfig.h"
#if CONFIG_HG_LVGL_SPIKE

#include "port_lvgl_spike.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"

namespace hgp::lvgl_spike {
namespace {

constexpr int kBufRows = 40;

hg::Display* g_d = nullptr;
hg::DisplayInfo g_info;
lv_display_t* g_disp = nullptr;

struct Counters {
  uint32_t frames = 0, flushes = 0;
  int64_t flush_us = 0, handler_us = 0, flushed_px = 0;
} g_c;

uint32_t tick_ms() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void flush_cb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
  const int64_t t0 = esp_timer_get_time();
  const int w = lv_area_get_width(a), h = lv_area_get_height(a);
  if (g_info.swap_bytes) lv_draw_sw_rgb565_swap(px, static_cast<uint32_t>(w) * h);
  uint16_t* fb = g_d->framebuffer();
  const auto* src = reinterpret_cast<const uint16_t*>(px);
  for (int y = 0; y < h; ++y)
    std::memcpy(fb + static_cast<size_t>(a->y1 + y) * g_info.width + a->x1, src + static_cast<size_t>(y) * w,
                static_cast<size_t>(w) * 2);
  g_d->flush(static_cast<uint16_t>(a->y1), static_cast<uint16_t>(a->y2 + 1));  // whole rows, as the UI does
  g_c.flush_us += esp_timer_get_time() - t0;
  g_c.flushed_px += static_cast<int64_t>(g_info.width) * h;
  ++g_c.flushes;
  lv_display_flush_ready(disp);
}

void render_ready_cb(lv_event_t*) { ++g_c.frames; }

struct Heap {
  size_t internal, psram;
  static Heap now() {
    return {heap_caps_get_free_size(MALLOC_CAP_INTERNAL), heap_caps_get_free_size(MALLOC_CAP_SPIRAM)};
  }
};

std::string init() {
  if (g_disp) return "";
  g_info = g_d->info();
  if (g_d->framebuffer_rows() != 0) return "banded framebuffer: not supported by the spike";
  lv_init();
  lv_tick_set_cb(tick_ms);
  g_disp = lv_display_create(g_info.width, g_info.height);
  const size_t bytes = static_cast<size_t>(g_info.width) * kBufRows * 2;
  void* b1 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  void* b2 = heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
  if (!b1 || !b2) return "no internal RAM for the draw buffers";
  lv_display_set_buffers(g_disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
  lv_display_set_flush_cb(g_disp, flush_cb);
  lv_display_add_event_cb(g_disp, render_ready_cb, LV_EVENT_RENDER_READY, nullptr);
  return "";
}

// --- scenes -----------------------------------------------------------------

void scroll_exec(void* obj, int32_t v) { lv_obj_scroll_to_y(static_cast<lv_obj_t*>(obj), v, LV_ANIM_OFF); }

lv_obj_t* scene_scroll() {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_t* list = lv_list_create(scr);
  lv_obj_set_size(list, LV_PCT(100), LV_PCT(100));
  char text[32];
  for (int i = 0; i < 60; ++i) {
    std::snprintf(text, sizeof text, "Item %d - Kitsune", i + 1);
    lv_list_add_button(list, i % 3 == 0 ? LV_SYMBOL_AUDIO : LV_SYMBOL_FILE, text);
  }
  lv_obj_update_layout(list);
  const int32_t max = lv_obj_get_scroll_bottom(list);
  lv_anim_t a;
  lv_anim_init(&a);
  lv_anim_set_var(&a, list);
  lv_anim_set_exec_cb(&a, scroll_exec);
  lv_anim_set_values(&a, 0, max > 0 ? max : 1);
  lv_anim_set_duration(&a, 3000);
  lv_anim_set_playback_duration(&a, 3000);
  lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
  lv_anim_start(&a);
  return scr;
}

lv_obj_t* g_chart = nullptr;
lv_chart_series_t* g_s1 = nullptr;
lv_chart_series_t* g_s2 = nullptr;
int g_step = 0;

void chart_tick(lv_timer_t*) {
  ++g_step;
  lv_chart_set_next_value(g_chart, g_s1, static_cast<int32_t>(50 + 40 * std::sin(g_step * 0.15)));
  lv_chart_set_next_value(g_chart, g_s2, static_cast<int32_t>(50 + 30 * std::cos(g_step * 0.07)));
}

lv_timer_t* g_chart_timer = nullptr;

lv_obj_t* scene_chart() {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_t* title = lv_label_create(scr);
  lv_label_set_text(title, "Gastos por dia");
  lv_obj_set_style_text_font(title, &lv_font_montserrat_20, 0);
  lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 8);
  g_chart = lv_chart_create(scr);
  lv_obj_set_size(g_chart, g_info.width - 20, g_info.height - 70);
  lv_obj_align(g_chart, LV_ALIGN_BOTTOM_MID, 0, -10);
  lv_chart_set_point_count(g_chart, 50);
  lv_chart_set_range(g_chart, LV_CHART_AXIS_PRIMARY_Y, 0, 100);
  g_s1 = lv_chart_add_series(g_chart, lv_palette_main(LV_PALETTE_ORANGE), LV_CHART_AXIS_PRIMARY_Y);
  g_s2 = lv_chart_add_series(g_chart, lv_palette_main(LV_PALETTE_CYAN), LV_CHART_AXIS_PRIMARY_Y);
  g_chart_timer = lv_timer_create(chart_tick, 33, nullptr);
  return scr;
}

lv_obj_t* scene_spinner() {
  lv_obj_t* scr = lv_obj_create(nullptr);
  lv_obj_t* sp = lv_spinner_create(scr);
  lv_spinner_set_anim_params(sp, 1000, 270);
  lv_obj_set_size(sp, 100, 100);
  lv_obj_center(sp);
  lv_obj_t* label = lv_label_create(scr);
  lv_label_set_text(label, "Pensando...");
  lv_obj_set_style_text_font(label, &lv_font_montserrat_20, 0);
  lv_obj_align(label, LV_ALIGN_BOTTOM_MID, 0, -30);
  return scr;
}

std::string measure(const char* name, lv_obj_t* (*make)(), int seconds) {
  lv_obj_t* scr = make();
  lv_screen_load(scr);
  lv_obj_invalidate(scr);
  const int64_t warm_end = esp_timer_get_time() + 300 * 1000;
  while (esp_timer_get_time() < warm_end) {
    lv_timer_handler();
    vTaskDelay(1);
  }
  g_c = {};
  const int64_t start = esp_timer_get_time(), end = start + static_cast<int64_t>(seconds) * 1000000;
  while (esp_timer_get_time() < end) {
    const int64_t t0 = esp_timer_get_time();
    lv_timer_handler();
    g_c.handler_us += esp_timer_get_time() - t0;
    vTaskDelay(1);  // let the idle task and the watchdog run
  }
  const double secs = (esp_timer_get_time() - start) / 1e6;
  if (g_chart_timer) {
    lv_timer_delete(g_chart_timer);
    g_chart_timer = nullptr;
  }
  lv_anim_delete_all();
  lv_obj_t* blank = lv_obj_create(nullptr);
  lv_screen_load(blank);
  lv_obj_delete(scr);
  const double fps = g_c.frames / secs;
  const double frame_ms = g_c.frames ? g_c.handler_us / 1000.0 / g_c.frames : 0;
  const double flush_ms = g_c.frames ? g_c.flush_us / 1000.0 / g_c.frames : 0;
  const double rows = g_c.frames ? static_cast<double>(g_c.flushed_px) / g_info.width / g_c.frames : 0;
  char line[200];
  std::snprintf(line, sizeof line,
                "@lvgl scene=%s fps=%.1f frame_ms=%.1f render_ms=%.1f flush_ms=%.1f rows_per_frame=%.0f frames=%u",
                name, fps, frame_ms, frame_ms - flush_ms, flush_ms, rows, static_cast<unsigned>(g_c.frames));
  return line;
}

}  // namespace

std::string run(hg::Display& display, std::string_view args) {
  g_d = &display;
  int seconds = 4;
  if (args.size() > 5) seconds = std::max(1, std::min(20, std::atoi(std::string(args.substr(5)).c_str())));
  const Heap before = Heap::now();
  const std::string err = init();
  if (!err.empty()) return "@error lvgl: " + err;
  const Heap after_init = Heap::now();
  std::string out;
  for (const auto& [name, make] : {std::pair<const char*, lv_obj_t* (*)()>{"scroll", scene_scroll},
                                   {"chart", scene_chart},
                                   {"spinner", scene_spinner}}) {
    out += measure(name, make, seconds);
    out += "\n";
    std::printf("%s", out.c_str() + out.rfind("@lvgl"));
  }
  const Heap end = Heap::now();
  char mem[200];
  std::snprintf(mem, sizeof mem,
                "@lvgl mem init_internal_kb=%d init_psram_kb=%d after_scenes_internal_kb=%d after_scenes_psram_kb=%d "
                "free_internal_kb=%d spi_mhz_override=%d",
                static_cast<int>((before.internal - after_init.internal) / 1024),
                static_cast<int>((before.psram - after_init.psram) / 1024),
                static_cast<int>((before.internal - end.internal) / 1024),
                static_cast<int>((before.psram - end.psram) / 1024), static_cast<int>(end.internal / 1024),
                CONFIG_HG_LCD_SPI_MHZ_OVERRIDE);
  std::printf("%s\n", mem);
  out += mem;
  return out;
}

}  // namespace hgp::lvgl_spike

#endif  // CONFIG_HG_LVGL_SPIKE
