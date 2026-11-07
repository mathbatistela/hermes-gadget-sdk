// The LVGL view of a panel (hg/panel.hpp). Built with HG_PANEL only.
//
// LVGL renders into one small draw buffer (40 rows) and the flush callback hands each
// area to ViewConfig::blit, which copies it into the port's framebuffer and pushes the
// rows with the port's own Display::flush: the path the device UI uses. LVGL keeps its
// own clock, set from render(now_ms), so the simulator and the tests draw the same
// pixels for the same inputs. Touches are read in event mode, one per sample, so a
// quick tap between two renders is not lost.
#if HG_PANEL

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "hg/panel.hpp"
#include "lvgl.h"

namespace hg::panel {
namespace {

// The device UI's palette (ui.cpp).
lv_color_t c(uint8_t r, uint8_t g, uint8_t b) { return lv_color_make(r, g, b); }
const lv_color_t kBg = c(10, 14, 20);
const lv_color_t kCard = c(24, 31, 42);
const lv_color_t kText = c(232, 238, 242);
const lv_color_t kDim = c(132, 146, 160);
const lv_color_t kFaint = c(52, 62, 76);
const lv_color_t kAccent = c(242, 179, 61);
const lv_color_t kGreen = c(61, 214, 140);
const lv_color_t kBlue = c(90, 169, 242);
const lv_color_t kRed = c(242, 95, 92);
const lv_color_t kYellow = c(242, 201, 76);

constexpr int kBufRows = 40;
constexpr int kHeaderH = 32;
constexpr int kPad = 8;
constexpr int kGap = 8;
constexpr uint32_t kDetailMs = 8000;

const char* const kSymClose = "\xEF\x80\x8D";  // U+F00D, in the panel fonts
const char* const kSymOk = "\xEF\x80\x8C";     // U+F00C
const char* const kSymRight = "\xEF\x81\x94";  // U+F054

uint32_t g_tick = 0;
uint32_t tick_cb() { return g_tick; }

lv_color_t tone_colour(Tone t, lv_color_t neutral) {
  switch (t) {
    case Tone::Ok: return kGreen;
    case Tone::Warn: return kYellow;
    case Tone::Bad: return kRed;
    case Tone::Accent: return kAccent;
    case Tone::Neutral: break;
  }
  return neutral;
}

// A plain box: no padding, border, background or scrolling of its own.
lv_obj_t* box(lv_obj_t* parent) {
  lv_obj_t* o = lv_obj_create(parent);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
  lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_add_flag(o, LV_OBJ_FLAG_EVENT_BUBBLE);
  lv_obj_set_style_pad_all(o, 0, 0);
  lv_obj_set_style_border_width(o, 0, 0);
  lv_obj_set_style_bg_opa(o, LV_OPA_TRANSP, 0);
  return o;
}

lv_obj_t* label(lv_obj_t* parent, const std::string& text, const lv_font_t* font, lv_color_t colour) {
  lv_obj_t* l = lv_label_create(parent);
  lv_label_set_text(l, text.c_str());
  lv_obj_set_style_text_font(l, font, 0);
  lv_obj_set_style_text_color(l, colour, 0);
  return l;
}

// A label that takes the room left in a row and ends in "..." when it doesn't fit.
lv_obj_t* grow_label(lv_obj_t* parent, const std::string& text, const lv_font_t* font, lv_color_t colour) {
  lv_obj_t* l = label(parent, text, font, colour);
  lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
  lv_obj_set_flex_grow(l, 1);
  lv_obj_set_height(l, lv_font_get_line_height(font));  // one line: DOTS cuts it there
  return l;
}

lv_obj_t* row(lv_obj_t* parent) {
  lv_obj_t* r = box(parent);
  lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
  lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
  lv_obj_set_style_pad_column(r, 6, 0);
  return r;
}

lv_obj_t* column(lv_obj_t* parent, int gap) {
  lv_obj_t* col = box(parent);
  lv_obj_set_size(col, LV_PCT(100), LV_SIZE_CONTENT);
  lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
  lv_obj_set_style_pad_row(col, gap, 0);
  return col;
}

void bar_style(lv_obj_t* bar, int height, lv_color_t colour) {
  lv_obj_set_size(bar, LV_PCT(100), height);
  lv_obj_remove_flag(bar, LV_OBJ_FLAG_CLICKABLE);
  lv_obj_set_style_radius(bar, height / 2, 0);
  lv_obj_set_style_bg_color(bar, kFaint, 0);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
  lv_obj_set_style_radius(bar, height / 2, LV_PART_INDICATOR);
  lv_obj_set_style_bg_color(bar, colour, LV_PART_INDICATOR);
  lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, LV_PART_INDICATOR);
}

int32_t permille(double value, double max) {
  if (max <= 0) return 0;
  return static_cast<int32_t>(std::lround(std::clamp(value / max, 0.0, 1.0) * 1000));
}

class LvglView final : public View {
 public:
  LvglView(const ViewConfig& cfg, void* buf) : cfg_(cfg), buf_(buf) {
    disp_ = lv_display_create(cfg_.area.width, cfg_.area.height);
    lv_display_set_user_data(disp_, this);
    lv_display_set_color_format(disp_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(disp_, buf_, nullptr, buf_bytes(cfg_), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(disp_, &LvglView::flush_cb);

    indev_ = lv_indev_create();
    lv_indev_set_type(indev_, LV_INDEV_TYPE_POINTER);
    lv_indev_set_display(indev_, disp_);
    lv_indev_set_user_data(indev_, this);
    lv_indev_set_read_cb(indev_, &LvglView::read_cb);
    lv_indev_set_mode(indev_, LV_INDEV_MODE_EVENT);
    lv_indev_set_long_press_time(indev_, 450);

    build_frame();
  }

  ~LvglView() override {
    lv_indev_delete(indev_);
    lv_display_delete(disp_);  // and every object on it
    std::free(buf_);
  }

  static size_t buf_bytes(const ViewConfig& cfg) {
    return static_cast<size_t>(cfg.area.width) * kBufRows * sizeof(uint16_t);
  }

  void show(const Spec& spec, bool keep_scroll) override {
    lv_display_set_default(disp_);
    const int32_t scroll = keep_scroll ? lv_obj_get_scroll_y(body_) : 0;
    spec_ = spec;
    hide_detail();
    lv_obj_clean(body_);
    targets_.clear();
    lv_label_set_text(title_, spec_.title.empty() ? "Hermes" : spec_.title.c_str());
    for (size_t i = 0; i < spec_.items.size(); ++i) build_item(spec_.items[i]);
    lv_obj_update_layout(screen_);
    lv_obj_scroll_to_y(body_, std::min<int32_t>(scroll, lv_obj_get_scroll_bottom(body_) + scroll), LV_ANIM_OFF);
    lv_obj_invalidate(screen_);
  }

  void hide() override {
    hide_detail();
    lv_obj_clean(body_);
    targets_.clear();
  }

  void render(uint32_t now_ms) override {
    g_tick = now_ms;
    if (detail_until_ && static_cast<int32_t>(now_ms - detail_until_) >= 0) hide_detail();
    lv_display_set_default(disp_);
    lv_timer_handler();
  }

  void invalidate() override { lv_obj_invalidate(screen_); }

  void touch(bool pressed, int x, int y, uint32_t now_ms) override {
    g_tick = now_ms;
    pressed_ = pressed;
    if (pressed) {
      x_ = std::clamp(x, 0, cfg_.area.width - 1);
      y_ = std::clamp(y, 0, cfg_.area.height - 1);
    }
    lv_indev_read(indev_);
  }

 private:
  // What a tappable object stands for. Owned here; LVGL holds a pointer.
  struct Target {
    LvglView* view;
    std::string item, value, label, detail;
    lv_obj_t* obj = nullptr;
  };

  static void flush_cb(lv_display_t* disp, const lv_area_t* a, uint8_t* px) {
    auto* self = static_cast<LvglView*>(lv_display_get_user_data(disp));
    const int w = lv_area_get_width(a), h = lv_area_get_height(a);
    if (self->cfg_.swap_bytes) lv_draw_sw_rgb565_swap(px, static_cast<uint32_t>(w) * h);
    if (self->cfg_.blit)
      self->cfg_.blit(self->cfg_.area.x + a->x1, self->cfg_.area.y + a->y1, w, h, reinterpret_cast<const uint16_t*>(px));
    lv_display_flush_ready(disp);
  }

  static void read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* self = static_cast<LvglView*>(lv_indev_get_user_data(indev));
    data->point.x = self->x_;
    data->point.y = self->y_;
    data->state = self->pressed_ ? LV_INDEV_STATE_PRESSED : LV_INDEV_STATE_RELEASED;
  }

  static void on_click(lv_event_t* e) {
    auto* t = static_cast<Target*>(lv_event_get_user_data(e));
    t->view->tapped(*t);
  }

  static void on_close_click(lv_event_t* e) {
    auto* self = static_cast<LvglView*>(lv_event_get_user_data(e));
    if (self->on_close) self->on_close();
  }

  // A long press anywhere that isn't a tappable entry or the close button.
  static void on_long_press(lv_event_t* e) {
    auto* self = static_cast<LvglView*>(lv_event_get_user_data(e));
    lv_obj_t* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    for (const auto& t : self->targets_)
      if (t->obj == target) return;
    if (self->on_hold) self->on_hold();
  }

  static void on_detail_click(lv_event_t* e) { static_cast<LvglView*>(lv_event_get_user_data(e))->hide_detail(); }

  void tapped(const Target& t) {
    if (!t.value.empty() && on_tap) on_tap(Tap{spec_.id, t.item, t.value, t.label});
    if (!t.detail.empty()) {
      show_detail(t.label.empty() ? t.detail : t.label + "\n" + t.detail);
    } else if (!t.value.empty()) {
      show_detail(std::string(kSymOk) + "  " + (t.label.empty() ? t.value : t.label) + " - " + cfg_.sent_text);
    }
  }

  void show_detail(const std::string& text) {
    lv_label_set_text(detail_text_, text.c_str());
    lv_obj_remove_flag(detail_, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(detail_);
    detail_until_ = g_tick + kDetailMs;
    if (!detail_until_) detail_until_ = 1;
  }

  void hide_detail() {
    if (!detail_) return;
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_HIDDEN);
    detail_until_ = 0;
  }

  void make_tappable(lv_obj_t* o, const Item& item, const Entry& e) {
    if (!e.tappable()) return;
    targets_.push_back(std::make_unique<Target>(Target{this, item.id, e.id, e.label, e.detail, o}));
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_PRESS_LOCK);  // a finger that slides off presses nothing
    lv_obj_add_event_cb(o, &LvglView::on_click, LV_EVENT_SHORT_CLICKED, targets_.back().get());
    lv_obj_set_style_bg_color(o, kFaint, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_STATE_PRESSED);
  }

  // The fixed parts: a title bar with a close button, the scrolling column, the detail strip.
  void build_frame() {
    lv_display_set_default(disp_);
    screen_ = lv_display_get_screen_active(disp_);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen_, kBg, 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen_, kText, 0);
    lv_obj_set_style_text_font(screen_, &hg_font_14, 0);
    lv_obj_set_flex_flow(screen_, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* header = box(screen_);
    lv_obj_set_size(header, LV_PCT(100), kHeaderH);
    lv_obj_set_flex_flow(header, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(header, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_left(header, kPad, 0);
    lv_obj_set_style_border_side(header, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(header, 1, 0);
    lv_obj_set_style_border_color(header, kFaint, 0);
    title_ = grow_label(header, "", &hg_font_18, kText);

    lv_obj_t* close = box(header);
    lv_obj_set_size(close, 40, kHeaderH);
    lv_obj_add_flag(close, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(close, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_ext_click_area(close, 6);
    lv_obj_set_style_bg_color(close, kFaint, LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(close, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_add_event_cb(close, &LvglView::on_close_click, LV_EVENT_CLICKED, this);
    lv_obj_t* x = label(close, kSymClose, &hg_font_18, kDim);
    lv_obj_center(x);

    body_ = lv_obj_create(screen_);
    lv_obj_set_width(body_, LV_PCT(100));
    lv_obj_set_flex_grow(body_, 1);
    lv_obj_set_style_border_width(body_, 0, 0);
    lv_obj_set_style_bg_opa(body_, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(body_, kPad, 0);
    lv_obj_set_style_pad_row(body_, kGap + 2, 0);
    lv_obj_set_flex_flow(body_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(body_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(body_, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_set_style_bg_color(body_, kFaint, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(body_, LV_OPA_COVER, LV_PART_SCROLLBAR);
    lv_obj_set_style_width(body_, 3, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(body_, 2, LV_PART_SCROLLBAR);
    lv_obj_set_style_pad_right(body_, 2, LV_PART_SCROLLBAR);
    lv_obj_add_event_cb(body_, &LvglView::on_long_press, LV_EVENT_LONG_PRESSED, this);

    detail_ = lv_obj_create(screen_);
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_FLOATING);
    lv_obj_remove_flag(detail_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(detail_, cfg_.area.width - 2 * kPad, LV_SIZE_CONTENT);
    lv_obj_align(detail_, LV_ALIGN_BOTTOM_MID, 0, -kPad);
    lv_obj_set_style_bg_color(detail_, kCard, 0);
    lv_obj_set_style_bg_opa(detail_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(detail_, kBlue, 0);
    lv_obj_set_style_border_width(detail_, 1, 0);
    lv_obj_set_style_radius(detail_, 6, 0);
    lv_obj_set_style_pad_all(detail_, kPad, 0);
    lv_obj_set_style_max_height(detail_, cfg_.area.height / 2, 0);
    lv_obj_add_event_cb(detail_, &LvglView::on_detail_click, LV_EVENT_CLICKED, this);
    detail_text_ = label(detail_, "", &hg_font_14, kText);
    lv_obj_set_width(detail_text_, LV_PCT(100));
    lv_label_set_long_mode(detail_text_, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_add_flag(detail_, LV_OBJ_FLAG_HIDDEN);
  }

  // An item's column in the body, with its heading when it has one.
  lv_obj_t* section(const Item& item, bool with_heading = true) {
    lv_obj_t* col = column(body_, 4);
    if (with_heading && !item.label.empty()) {
      lv_obj_t* l = label(col, item.label, &hg_font_12, kDim);
      lv_obj_set_width(l, LV_PCT(100));
      lv_obj_set_height(l, lv_font_get_line_height(&hg_font_12));
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
    }
    return col;
  }

  void build_item(const Item& item) {
    switch (item.kind) {
      case Kind::Text: return build_text(item);
      case Kind::Stats: return build_stats(item);
      case Kind::Progress: return item.arc ? build_arc(item) : build_progress_bar(item);
      case Kind::Bars: return build_bars(item);
      case Kind::Chart: return build_chart(item);
      case Kind::List: return build_list(item);
      case Kind::Buttons: return build_buttons(item);
    }
  }

  void build_text(const Item& item) {
    const lv_font_t* font = item.style == Item::Style::Title ? &hg_font_18 : &hg_font_14;
    lv_obj_t* l = label(section(item, false), item.text, font, item.style == Item::Style::Dim ? kDim : kText);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_WRAP);
  }

  void build_stats(const Item& item) {
    lv_obj_t* grid = box(section(item));
    lv_obj_set_size(grid, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(grid, 6, 0);
    lv_obj_set_style_pad_row(grid, 6, 0);
    const int inner = cfg_.area.width - 2 * kPad;
    const bool one = item.entries.size() == 1;
    for (const Entry& e : item.entries) {
      lv_obj_t* tile = box(grid);
      lv_obj_set_size(tile, one ? inner : (inner - 6) / 2, LV_SIZE_CONTENT);
      lv_obj_set_flex_flow(tile, LV_FLEX_FLOW_COLUMN);
      lv_obj_set_style_pad_all(tile, kPad, 0);
      lv_obj_set_style_pad_row(tile, 2, 0);
      lv_obj_set_style_radius(tile, 6, 0);
      lv_obj_set_style_bg_color(tile, kCard, 0);
      lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
      lv_obj_t* l = label(tile, e.label, &hg_font_12, kDim);
      lv_obj_set_size(l, LV_PCT(100), lv_font_get_line_height(&hg_font_12));
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_t* v = label(tile, e.value, &hg_font_18, tone_colour(e.tone, kText));
      lv_obj_set_size(v, LV_PCT(100), lv_font_get_line_height(&hg_font_18));
      lv_label_set_long_mode(v, LV_LABEL_LONG_MODE_DOTS);
      make_tappable(tile, item, e);
    }
  }

  void build_arc(const Item& item) {
    lv_obj_t* col = section(item, false);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    const int size = std::min(132, cfg_.area.width - 4 * kPad);
    lv_obj_t* arc = lv_arc_create(col);
    lv_obj_set_size(arc, size, size);
    lv_obj_remove_flag(arc, LV_OBJ_FLAG_CLICKABLE);
    lv_arc_set_rotation(arc, 135);
    lv_arc_set_bg_angles(arc, 0, 270);
    lv_arc_set_range(arc, 0, 1000);
    lv_arc_set_value(arc, permille(item.value, item.max));
    lv_obj_set_style_arc_width(arc, 12, LV_PART_MAIN);
    lv_obj_set_style_arc_color(arc, kFaint, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_MAIN);
    lv_obj_set_style_arc_width(arc, 12, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(arc, tone_colour(item.tone, kAccent), LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(arc, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(arc, 0, LV_PART_KNOB);
    lv_obj_t* figure = label(arc, item.text, &hg_font_26, kText);
    lv_obj_set_size(figure, size - 36, lv_font_get_line_height(&hg_font_26));
    lv_obj_set_style_text_align(figure, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(figure, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_center(figure);
    if (!item.label.empty()) {  // in the arc's open bottom
      lv_obj_t* l = label(arc, item.label, &hg_font_14, kDim);
      lv_obj_set_size(l, size - 8, lv_font_get_line_height(&hg_font_14));
      lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, 0);
    }
  }

  void build_progress_bar(const Item& item) {
    lv_obj_t* col = section(item, false);
    lv_obj_t* r = row(col);
    grow_label(r, item.label, &hg_font_14, kText);
    label(r, item.text, &hg_font_14, kDim);
    lv_obj_t* bar = lv_bar_create(col);
    bar_style(bar, 8, tone_colour(item.tone, kAccent));
    lv_bar_set_range(bar, 0, 1000);
    lv_bar_set_value(bar, permille(item.value, item.max), LV_ANIM_OFF);
  }

  void build_bars(const Item& item) {
    double top = 0;
    for (const Entry& e : item.entries) top = std::max(top, e.amount);
    lv_obj_t* col = section(item);
    lv_obj_set_style_pad_row(col, 2, 0);
    for (const Entry& e : item.entries) {
      lv_obj_t* entry = column(col, 3);
      lv_obj_set_style_pad_ver(entry, 3, 0);
      lv_obj_set_style_radius(entry, 4, 0);
      lv_obj_t* r = row(entry);
      grow_label(r, e.label, &hg_font_14, kText);
      label(r, e.value, &hg_font_14, kDim);
      lv_obj_t* bar = lv_bar_create(entry);
      bar_style(bar, 6, tone_colour(e.tone, kBlue));
      lv_bar_set_range(bar, 0, 1000);
      lv_bar_set_value(bar, permille(e.amount, top), LV_ANIM_OFF);
      make_tappable(entry, item, e);
    }
  }

  void build_chart(const Item& item) {
    lv_obj_t* col = section(item);
    double lo = item.points[0], hi = item.points[0];
    for (double v : item.points) lo = std::min(lo, v), hi = std::max(hi, v);
    if (!item.line) lo = std::min(lo, 0.0);  // columns stand on zero
    const double pad = hi > lo ? (hi - lo) * 0.1 : std::max(1.0, std::fabs(hi) * 0.1);
    const double y0 = item.line ? lo - pad : lo, y1 = hi + pad;
    lv_obj_t* chart = lv_chart_create(col);
    lv_obj_set_size(chart, LV_PCT(100), 110);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(chart, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(chart, kCard, 0);
    lv_obj_set_style_bg_opa(chart, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chart, 6, 0);
    lv_obj_set_style_border_width(chart, 0, 0);
    lv_obj_set_style_pad_all(chart, kPad, 0);
    lv_obj_set_style_line_color(chart, kFaint, 0);
    lv_obj_set_style_line_width(chart, 1, 0);
    lv_chart_set_div_line_count(chart, 3, 0);
    lv_chart_set_type(chart, item.line ? LV_CHART_TYPE_LINE : LV_CHART_TYPE_BAR);
    lv_chart_set_point_count(chart, static_cast<uint32_t>(item.points.size()));
    lv_chart_set_axis_range(chart, LV_CHART_AXIS_PRIMARY_Y, 0, 1000);
    lv_chart_set_update_mode(chart, LV_CHART_UPDATE_MODE_SHIFT);
    lv_obj_set_style_line_width(chart, 2, LV_PART_ITEMS);
    lv_obj_set_style_width(chart, 0, LV_PART_INDICATOR);  // no dots on the line
    lv_obj_set_style_height(chart, 0, LV_PART_INDICATOR);
    lv_obj_set_style_pad_column(chart, 2, 0);
    lv_obj_set_style_radius(chart, 2, LV_PART_ITEMS);
    lv_chart_series_t* s = lv_chart_add_series(chart, item.line ? kBlue : kAccent, LV_CHART_AXIS_PRIMARY_Y);
    for (size_t i = 0; i < item.points.size(); ++i)
      lv_chart_set_series_value_by_id(chart, s, static_cast<uint32_t>(i),
                                      static_cast<int32_t>(std::lround((item.points[i] - y0) / (y1 - y0) * 1000)));
    lv_chart_refresh(chart);
    if (item.labels.size() >= 2) {
      lv_obj_t* r = row(col);
      grow_label(r, item.labels.front(), &hg_font_12, kDim);
      label(r, item.labels.back(), &hg_font_12, kDim);
    }
  }

  void build_list(const Item& item) {
    lv_obj_t* list = column(section(item), 0);
    lv_obj_set_style_bg_color(list, kCard, 0);
    lv_obj_set_style_bg_opa(list, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(list, 6, 0);
    lv_obj_set_style_clip_corner(list, true, 0);
    for (size_t i = 0; i < item.entries.size(); ++i) {
      const Entry& e = item.entries[i];
      lv_obj_t* r = row(list);
      lv_obj_set_style_pad_hor(r, kPad, 0);
      lv_obj_set_style_pad_ver(r, 9, 0);
      lv_obj_set_style_pad_column(r, 8, 0);
      if (i + 1 < item.entries.size()) {
        lv_obj_set_style_border_side(r, LV_BORDER_SIDE_BOTTOM, 0);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, kFaint, 0);
      }
      if (e.tone != Tone::Neutral) {
        lv_obj_t* dot = box(r);
        lv_obj_set_size(dot, 8, 8);
        lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(dot, tone_colour(e.tone, kDim), 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
      }
      grow_label(r, e.label, &hg_font_14, kText);
      if (!e.value.empty()) label(r, e.value, &hg_font_14, tone_colour(e.tone, kDim));
      if (!e.id.empty()) label(r, kSymRight, &hg_font_12, kFaint);
      make_tappable(r, item, e);
    }
  }

  void build_buttons(const Item& item) {
    lv_obj_t* r = box(section(item));
    lv_obj_set_size(r, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(r, 6, 0);
    lv_obj_set_style_pad_row(r, 6, 0);
    const bool fill = item.entries.size() <= 3;
    for (const Entry& e : item.entries) {
      lv_obj_t* b = box(r);
      lv_obj_set_height(b, 40);
      lv_obj_set_style_min_width(b, 64, 0);
      if (fill) lv_obj_set_flex_grow(b, 1);
      else lv_obj_set_width(b, LV_SIZE_CONTENT);
      lv_obj_set_style_pad_hor(b, 12, 0);
      lv_obj_set_style_radius(b, 6, 0);
      const bool plain = e.tone == Tone::Neutral;
      lv_obj_set_style_bg_color(b, plain ? kCard : tone_colour(e.tone, kCard), 0);
      lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(b, plain ? 1 : 0, 0);
      lv_obj_set_style_border_color(b, kFaint, 0);
      lv_obj_t* l = label(b, e.label, &hg_font_14, plain ? kText : kBg);
      lv_label_set_long_mode(l, LV_LABEL_LONG_MODE_DOTS);
      lv_obj_set_style_max_width(l, cfg_.area.width - 2 * kPad - 24, 0);
      lv_obj_center(l);
      make_tappable(b, item, e);
      lv_obj_set_style_bg_color(b, plain ? kFaint : kText, LV_STATE_PRESSED);
    }
  }

  ViewConfig cfg_;
  void* buf_;
  lv_display_t* disp_ = nullptr;
  lv_indev_t* indev_ = nullptr;
  lv_obj_t* screen_ = nullptr;
  lv_obj_t* title_ = nullptr;
  lv_obj_t* body_ = nullptr;
  lv_obj_t* detail_ = nullptr;
  lv_obj_t* detail_text_ = nullptr;
  uint32_t detail_until_ = 0;
  Spec spec_;
  std::vector<std::unique_ptr<Target>> targets_;
  bool pressed_ = false;
  int x_ = 0, y_ = 0;
};

}  // namespace

bool available() { return true; }

std::unique_ptr<View> make_view(const ViewConfig& config) {
  if (config.area.width <= 0 || config.area.height <= 0) return nullptr;
  static bool initialised = false;
  if (!initialised) {
    lv_init();
    lv_tick_set_cb(tick_cb);
    initialised = true;
  }
  const size_t bytes = LvglView::buf_bytes(config);
  void* buf = config.alloc ? config.alloc(bytes) : std::malloc(bytes);
  if (!buf) return nullptr;
  return std::make_unique<LvglView>(config, buf);
}

}  // namespace hg::panel

#endif  // HG_PANEL
