// Panels: a small interactive screen Hermes describes in JSON (docs/protocol.md#panels)
// and the device draws with LVGL. A panel is a title over a column of items: text,
// stat tiles, a progress arc or bar, ranked bars, a chart, a list and buttons. A tap on
// an entry with an `id` goes back to the agent as a `panel.tap` event; an entry with a
// `detail` shows it on the device, with no round trip.
//
// This header has no LVGL in it: parse() and the limits build everywhere, so the spec
// is tested on every host build. The LVGL view (panel_lvgl.cpp) builds with HG_PANEL.
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "hg/hal.hpp"
#include "hg/json.hpp"

namespace hg::panel {

// The newest panel layout this firmware draws (caps.panel.v).
constexpr int kVersion = 1;

// What a panel may hold. Bigger panels are cut to these, not refused.
constexpr size_t kMaxItems = 12;
constexpr size_t kMaxEntries = 8;       // stats, bars and buttons
constexpr size_t kMaxListRows = 16;
constexpr size_t kMaxPoints = 32;       // a chart's values
constexpr size_t kMaxText = 160;        // a label, value or text item, in bytes
constexpr size_t kMaxDetail = 320;      // an entry's detail, in bytes
constexpr size_t kMaxId = 48;

enum class Tone : uint8_t { Neutral, Ok, Warn, Bad, Accent };

// One row of a list, one bar, one stat tile or one button.
struct Entry {
  std::string id;      // a tap sends panel.tap with this value; empty: not sent
  std::string label;
  std::string value;   // the text shown: a bar's or row's figure at the right, a stat's number (`text`, else `value`)
  std::string detail;  // shown on the device when tapped; empty: nothing
  double amount = 0;   // a bar's length
  Tone tone = Tone::Neutral;
  bool tappable() const { return !id.empty() || !detail.empty(); }
};

enum class Kind : uint8_t { Text, Stats, Progress, Bars, Chart, List, Buttons };

struct Item {
  Kind kind = Kind::Text;
  std::string id;     // names the item in panel.tap
  std::string label;  // a heading over the item (bars, chart, list) or under it (progress)
  std::string text;   // a text item's text; progress: the figure in the middle
  enum class Style : uint8_t { Body, Title, Dim } style = Style::Body;  // text
  double value = 0, max = 100;  // progress
  bool arc = false;             // progress: an arc rather than a bar
  Tone tone = Tone::Accent;     // progress
  bool line = true;             // chart: a line, else columns
  std::vector<double> points;   // chart
  std::vector<std::string> labels;  // chart: the first and last are drawn under it
  std::vector<Entry> entries;   // stats, bars, list, buttons
};

struct Spec {
  std::string id;
  std::string title;
  uint32_t ttl_ms = 0;  // 0: until closed
  std::vector<Item> items;
};

// Parses a `panel` message. False with `error` set when nothing could be drawn:
// no id, or no item of a known type. Unknown item types and fields are skipped.
bool parse(const json::Value& msg, Spec& out, std::string& error);

// A tap that goes back to the agent.
struct Tap {
  std::string panel, item, value, label;
};

// The LVGL view of one panel, drawing into a rectangle of the display. One at a time.
class View {
 public:
  virtual ~View() = default;
  // Shows `spec`, replacing what was shown. `keep_scroll` keeps the column where it was
  // (an update of the same panel).
  virtual void show(const Spec& spec, bool keep_scroll) = 0;
  virtual void hide() = 0;
  // Draws what changed. `now_ms` drives LVGL's animations. Call often while it is on screen.
  virtual void render(uint32_t now_ms) = 0;
  // Everything is drawn again on the next render (something else drew over the area).
  virtual void invalidate() = 0;
  // A touch inside the area, in area coordinates; `pressed` false when the finger lifts.
  virtual void touch(bool pressed, int x, int y, uint32_t now_ms) = 0;
  // A tap on an entry with an id.
  std::function<void(const Tap&)> on_tap;
  // The close button.
  std::function<void()> on_close;
  // A finger held still on the panel, not on anything tappable: the app may talk. The lift
  // that ends it comes through touch() as usual.
  std::function<void()> on_hold;
};

// Where the view draws: a rectangle of the display, in panel coordinates.
struct Area {
  int x = 0, y = 0, width = 0, height = 0;
};

struct ViewConfig {
  Area area;
  bool swap_bytes = false;
  // Writes `rows` rows of `width` pixels (RGB565, already swapped as the display wants) at
  // (x, y) in panel coordinates, then pushes those rows to the panel.
  std::function<void(int x, int y, int width, int rows, const uint16_t* px)> blit;
  // The text shown after a tap goes to the agent ("Sent to Hermes").
  std::string sent_text = "Sent";
  // Draw buffer allocation (internal RAM on the ESP32); null: malloc.
  void* (*alloc)(size_t) = nullptr;
};

// Whether this firmware was built with panels (HG_PANEL).
bool available();
// The LVGL view, or null without HG_PANEL or without memory for it.
std::unique_ptr<View> make_view(const ViewConfig& config);

}  // namespace hg::panel
