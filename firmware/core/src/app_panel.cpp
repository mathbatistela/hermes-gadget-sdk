// Panels (docs/protocol.md#panels): Hermes sends a `panel`, the device draws it with
// LVGL between the bars, and a tap on an entry with an id goes back as a `panel.tap`
// event that reaches the agent. The panel is an overlay like a card or an image: a
// question, a recording or the alarm take the screen over, and it comes back after.
#include <algorithm>

#include "hg/app.hpp"

namespace hg {

namespace {

// A panel to try the view on the bench without Hermes: the console's `panel demo`.
const char* const kDemoPanel = R"({"type":"panel","id":"demo","title":"Orçamento de novembro","items":[
{"type":"progress","style":"arc","label":"gasto do mês","value":3401,"max":5500,"text":"62%"},
{"type":"stats","items":[{"label":"Gasto","value":"R$ 3.401"},{"label":"Sobra","value":"R$ 2.099","state":"ok"},
 {"label":"Hoje","value":"R$ 82"},{"label":"Média/dia","value":"R$ 486","state":"warn"}]},
{"type":"bars","id":"categoria","label":"Por categoria","items":[
 {"id":"mercado","label":"Mercado","value":1210,"text":"R$ 1.210","detail":"14 compras, a maior R$ 312 no Atacadão"},
 {"id":"restaurante","label":"Restaurante","value":820,"text":"R$ 820"},
 {"id":"transporte","label":"Transporte","value":510,"text":"R$ 510"},
 {"id":"casa","label":"Casa","value":460,"text":"R$ 460","state":"warn"}]},
{"type":"chart","label":"Gasto por dia","style":"columns","values":[120,80,310,95,60,240,82],
 "labels":["01/11","07/11"]},
{"type":"list","id":"alertas","label":"Avisos","items":[
 {"label":"Casa acima da média","value":"+38%","state":"warn","detail":"Conta de luz veio R$ 190 a mais"},
 {"id":"assinaturas","label":"Assinaturas","value":"R$ 129"}]},
{"type":"buttons","id":"acao","items":[{"id":"detalhar","label":"Detalhar","state":"primary"},
 {"id":"fechar_mes","label":"Fechar mês"}]}]})";

// What `panel test` opens over the demo, as the answer to a tap on "Mercado" would be.
const char* const kDemoDetailPanel = R"({"type":"panel","id":"demo-mercado","title":"Mercado","items":[
{"type":"stats","items":[{"label":"Compras","value":"14"},{"label":"Maior","value":"R$ 312"}]},
{"type":"list","id":"compra","label":"Últimas","items":[{"id":"c1","label":"Atacadão","value":"R$ 312"},
 {"id":"c2","label":"Feira","value":"R$ 64"}]}]})";

}  // namespace

bool App::panels_offered() const {
  return panel::available() && !panel_failed_ && profile_.touch_screen && hal_.display && ui_ &&
         hal_.display->framebuffer_rows() == 0 && !hal_.display->info().round;
}

panel::Area App::panel_area() const {
  const UiLayout& l = ui_->layout();
  return {0, l.main_y, ui_->area().width, l.main_h};
}

bool App::panel_hit(int x, int y) const {
  if (!panel_showing() || !panel_view_) return false;
  const panel::Area a = panel_area();
  return x >= a.x && x < a.x + a.width && y >= a.y && y < a.y + a.height;
}

void App::panel_touch(bool pressed, int x, int y) {
  if (!panel_view_) return;
  activity_at_ = now();
  const panel::Area a = panel_area();
  panel_view_->touch(pressed, x - a.x, y - a.y, now());
  if (!pressed && panel_talk_) {
    panel_talk_ = false;
    on_button(Button::Talk, false, true);
  }
}

void App::h_panel(const json::Value& m) {
  panel::Spec spec;
  std::string error;
  if (!panel::parse(m, spec, error)) {
    log(LogLevel::Warn, "panel refused: " + error);
    return;
  }
  show_panel(std::move(spec));
}

void App::h_panel_close(const json::Value& m) {
  const std::string& id = m["id"].as_string();
  if (overlay_ != Overlay::Panel) return;
  if (id.empty() || id == panel_spec_.id) {
    dismiss_overlay();
    return;
  }
  // An earlier panel, still under this one: the back arrow no longer returns to it.
  const size_t before = panel_back_.size();
  panel_back_.erase(std::remove_if(panel_back_.begin(), panel_back_.end(),
                                   [&](const PanelBack& b) { return b.spec.id == id; }),
                    panel_back_.end());
  if (panel_back_.size() != before) panel_view_->set_back(!panel_back_.empty());
}

bool App::show_panel(panel::Spec spec) {
  if (!panels_offered()) {
    log(LogLevel::Warn, "panel refused: this device draws no panels");
    return false;
  }
  if (!panel_view_) {
    panel::ViewConfig cfg;
    cfg.area = panel_area();
    // The canvas swaps bytes for the display itself, so LVGL's pixels go to it as they are.
    cfg.swap_bytes = false;
    cfg.blit = [this](int x, int y, int w, int rows, const uint16_t* px) {
      const panel::Area a = panel_area();
      Canvas c = ui_->canvas();
      c.set_clip_rows(a.y, a.y + a.height);
      c.blit(x, y, w, rows, px);
      ui_->flush(std::max(y, a.y), std::min(y + rows, a.y + a.height));
    };
    cfg.sent_text = tr(i18n::Msg::PanelSent);
    cfg.alloc = profile_.panel_alloc;
    panel_view_ = panel::make_view(cfg);
    if (!panel_view_) {
      panel_failed_ = true;  // and caps.panel goes from the next hello
      log(LogLevel::Warn, "panel refused: no memory for the panel view");
      return false;
    }
    panel_view_->on_tap = [this](const panel::Tap& t) {
      json::Value data = json::Value::object();
      data.set("panel", t.panel);
      if (!t.item.empty()) data.set("item", t.item);
      data.set("value", t.value);
      if (!t.label.empty()) data.set("label", t.label);
      emit_event("panel.tap", std::move(data), true);
    };
    panel_view_->on_hold = [this] {  // hold to talk, as on the other screens
      if (!touch_presses_talk() || panel_talk_) return;
      panel_talk_ = true;
      on_button(Button::Talk, true, true);
    };
    panel_view_->on_close = [this] {
      dismiss_overlay();
      update_model();
    };
    panel_view_->on_back = [this] { panel_back(); };
  }
  if (alarm_) dismiss_alarm();  // a panel replaces the alarm, as a card does
  close_setup_screens();
  wake_display();
  if (settings_open()) close_settings();
  // The same panel again is an update: it keeps its place in the column. Another panel over
  // one on screen (the answer to a tap, usually) keeps the one under it for the back arrow.
  const bool showing = overlay_ == Overlay::Panel;
  const bool update = showing && spec.id == panel_spec_.id;
  std::vector<PanelBack> back;
  if (showing && !update) {
    back = std::move(panel_back_);
    const auto again = std::find_if(back.begin(), back.end(), [&](const PanelBack& b) { return b.spec.id == spec.id; });
    if (again != back.end()) {
      back.erase(again, back.end());  // an earlier panel sent again: back to it, not a loop
    } else {
      back.push_back(PanelBack{std::move(panel_spec_), panel_view_->scroll()});
      if (back.size() > panel::kMaxBack) back.erase(back.begin());
    }
  }
  if (!update) {
    dismiss_overlay();
    panel_back_ = std::move(back);
  }
  panel_spec_ = std::move(spec);
  panel_view_->show(panel_spec_, update);
  panel_view_->set_back(!panel_back_.empty());
  panel_drawn_ = false;
  overlay_ = Overlay::Panel;
  overlay_until_ = panel_spec_.ttl_ms ? now() + panel_spec_.ttl_ms : 0;
  return true;
}

bool App::panel_back() {
  if (overlay_ != Overlay::Panel || panel_back_.empty() || !panel_view_) return false;
  PanelBack b = std::move(panel_back_.back());
  panel_back_.pop_back();
  json::Value data = json::Value::object();
  data.set("panel", b.spec.id).set("from", panel_spec_.id);
  panel_spec_ = std::move(b.spec);
  panel_view_->show(panel_spec_, false);
  panel_view_->scroll_to(b.scroll);
  panel_view_->set_back(!panel_back_.empty());
  overlay_until_ = 0;  // its own ttl_s ran from when it was sent; it stays until closed
  emit_event("panel.back", std::move(data), false);  // for the logs; the agent isn't asked anything
  return true;
}

namespace {
constexpr int kTestDrags = 6;            // three scrolling down the column, three back up
constexpr uint32_t kTestDragMs = 500;    // finger moving
constexpr uint32_t kTestSettleMs = 1200; // momentum dies down
constexpr int kTestScroll = 120;         // where the demo is left before the panel over it
}  // namespace

// One step of `panel test`, every tick while it runs. Steps 0..2*kTestDrags-1 alternate a drag
// and its settling; then a tap on the close button, then the verdict.
void App::panel_test_step() {
  PanelTest& t = panel_test_;
  const uint32_t now_ms = now();
  const panel::Area a = panel_area();
  const uint32_t in_step = now_ms - t.step_at;
  auto next = [&] {
    ++t.step;
    t.step_at = now_ms;
  };
  if (t.step < 2 * kTestDrags) {
    const int drag = t.step / 2;
    const bool down = drag < kTestDrags / 2;  // the finger moves up: the column scrolls down
    const int y_from = down ? a.y + a.height - 40 : a.y + 60, y_to = down ? a.y + 60 : a.y + a.height - 40;
    if (t.step % 2 == 0) {
      const uint32_t ms = std::min(in_step, kTestDragMs);
      const int y = y_from + static_cast<int>((static_cast<int64_t>(y_to - y_from) * ms) / kTestDragMs);
      panel_touch(true, a.x + a.width / 2 - 30, y);
      if (in_step >= kTestDragMs) {
        panel_touch(false, a.x + a.width / 2 - 30, y);
        next();
      }
    } else if (in_step >= kTestSettleMs) {
      next();
    }
    return;
  }
  constexpr int kDone = 2 * kTestDrags;  // the scrolling part is over
  if (t.step == kDone) {  // measure it; then a panel over the demo, as a tap's answer brings one
    const panel::View::Stats s = panel_view_->stats();
    const uint32_t frames = s.frames - t.stats0.frames;
    const double secs = (now_ms - t.started) / 1000.0;
    json::Value r = json::Value::object();
    r.set("seconds", secs).set("frames", frames).set("fps", secs > 0 ? frames / secs : 0.0);
    r.set("draw_ms_per_frame", frames ? t.draw_us / 1000.0 / frames : 0.0);
    r.set("draw_ms_max", t.draw_max_us / 1000.0);
    r.set("rows_per_frame", frames ? static_cast<double>(s.rows - t.stats0.rows) / frames : 0.0);
    panel_test_result_ = r;
    panel_view_->scroll_to(kTestScroll);
    t.scroll = panel_view_->scroll();
    json::Value msg;
    json::parse(kDemoDetailPanel, msg);
    panel::Spec spec;
    std::string error;
    if (panel::parse(msg, spec, error)) show_panel(std::move(spec));
    next();
    return;
  }
  // Taps at the header's two ends: the back arrow, then the close button.
  const int back_x = a.x + 18, close_x = a.x + a.width - 20, bar_y = a.y + 16;
  if (t.step == kDone + 1) {
    if (in_step >= 600) {
      panel_touch(true, back_x, bar_y);
      next();
    }
    return;
  }
  if (t.step == kDone + 2) {
    if (in_step >= 80) {
      panel_touch(false, back_x, bar_y);
      next();
    }
    return;
  }
  if (t.step == kDone + 3) {
    if (in_step >= 400) {
      const bool back = overlay_ == Overlay::Panel && panel_spec_.id == "demo" && panel_back_.empty();
      panel_test_result_.set("back_by_tap", back).set("scroll_kept", back && panel_view_->scroll() == t.scroll);
      panel_touch(true, close_x, bar_y);
      next();
    }
    return;
  }
  if (t.step == kDone + 4) {
    if (in_step >= 80) {
      panel_touch(false, close_x, bar_y);
      next();
    }
    return;
  }
  if (in_step >= 400) {
    const bool closed = overlay_ != Overlay::Panel;
    panel_test_result_.set("closed_by_tap", closed);
    panel_test_result_.set("ok", closed && panel_test_result_["back_by_tap"].as_bool(false) &&
                                     panel_test_result_["scroll_kept"].as_bool(false) &&
                                     panel_test_result_["frames"].as_int() > 0);
    log(LogLevel::Info, "@panel_test " + panel_test_result_.dump());
    t.step = -1;
    update_model();
  }
}

void App::panel_tick() {
  if (!panel_view_) return;
  if (panel_test_.step >= 0) {
    if (overlay_ != Overlay::Panel && panel_test_.step <= 2 * kTestDrags + 3) {  // before the close tap
      panel_test_result_ = json::Value::object();
      panel_test_result_.set("ok", false).set("error", "the panel went away during the test");
      log(LogLevel::Warn, "@panel_test " + panel_test_result_.dump());
      panel_test_.step = -1;
    } else {
      panel_test_step();
    }
  }
  if (overlay_ != Overlay::Panel) {
    if (!panel_spec_.id.empty()) {  // a card or an image replaced it
      panel_view_->hide();
      panel_spec_ = panel::Spec();
    }
    panel_drawn_ = false;
    return;
  }
  if (model_.screen != Screen::Panel) {  // a question, a recording or a screen of the app over it
    panel_drawn_ = false;
    return;
  }
  if (!panel_drawn_) {
    panel_view_->invalidate();  // the area holds what was drawn over it
    panel_drawn_ = true;
  }
  if (panel_test_.step < 0) {
    panel_view_->render(now());
    return;
  }
  const uint64_t t0 = hal_.system->now_us();
  panel_view_->render(now());
  const uint64_t us = hal_.system->now_us() - t0;
  panel_test_.draw_us += us;
  panel_test_.draw_max_us = std::max(panel_test_.draw_max_us, us);
  ++panel_test_.draws;
}

std::string App::panel_command(std::string_view args) {
  if (!panels_offered()) return "@error this device draws no panels";
  if (args.empty()) {
    json::Value r = json::Value::object();
    r.set("showing", overlay_ == Overlay::Panel);
    if (overlay_ == Overlay::Panel) {
      r.set("id", panel_spec_.id).set("items", static_cast<int>(panel_spec_.items.size()));
      json::Value back = json::Value::array();
      for (const PanelBack& b : panel_back_) back.push(b.spec.id);
      r.set("back", back);
    }
    const panel::Area a = panel_area();  // where touches land on it, for bench automation
    json::Value area = json::Value::array();
    area.push(a.x).push(a.y).push(a.width).push(a.height);
    r.set("area", area);
    if (panel_test_.step >= 0) r.set("test", "running");
    else if (!panel_test_result_.is_null()) r.set("test", panel_test_result_);
    return "@panel " + r.dump();
  }
  if (args == "test") {
    if (panel_test_.step >= 0) return "@error a panel test is running";
    json::Value msg;
    json::parse(kDemoPanel, msg);
    panel::Spec spec;
    std::string error;
    if (!panel::parse(msg, spec, error) || !show_panel(std::move(spec))) return "@error the panel could not be shown";
    update_model();
    if (model_.screen != Screen::Panel) {
      dismiss_overlay();
      return "@error the panel is not on screen (offline, or something else is)";
    }
    panel_test_ = PanelTest();
    panel_test_.step = 0;
    panel_test_.started = panel_test_.step_at = now();
    panel_test_.stats0 = panel_view_->stats();
    panel_test_result_ = json::Value();
    return "@ok panel test started: about 12 s; the result is logged as @panel_test and shown by `panel`";
  }
  if (args == "back") {
    if (!panel_back()) return "@error no earlier panel to go back to";
    update_model();
    return "@ok back to " + panel_spec_.id;
  }
  if (args == "close") {
    if (overlay_ == Overlay::Panel) dismiss_overlay();
    update_model();
    return "@ok panel closed";
  }
  json::Value msg;
  std::string error;
  if (!json::parse(args == "demo" ? std::string_view(kDemoPanel) : args, msg, &error)) return "@error " + error;
  panel::Spec spec;
  if (!panel::parse(msg, spec, error)) return "@error " + error;
  const std::string id = spec.id;
  if (!show_panel(std::move(spec))) return "@error the panel could not be shown";
  update_model();
  return "@ok panel " + id;
}

}  // namespace hg
