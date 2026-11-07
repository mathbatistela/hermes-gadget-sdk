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
  if (overlay_ == Overlay::Panel && (id.empty() || id == panel_spec_.id)) dismiss_overlay();
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
  }
  if (alarm_) dismiss_alarm();  // a panel replaces the alarm, as a card does
  close_setup_screens();
  wake_display();
  if (settings_open()) close_settings();
  // The same panel again is an update: it keeps its place in the column.
  const bool update = overlay_ == Overlay::Panel && spec.id == panel_spec_.id;
  if (!update) dismiss_overlay();
  panel_spec_ = std::move(spec);
  panel_view_->show(panel_spec_, update);
  panel_drawn_ = false;
  overlay_ = Overlay::Panel;
  overlay_until_ = panel_spec_.ttl_ms ? now() + panel_spec_.ttl_ms : 0;
  return true;
}

void App::panel_tick() {
  if (!panel_view_) return;
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
  panel_view_->render(now());
}

std::string App::panel_command(std::string_view args) {
  if (!panels_offered()) return "@error this device draws no panels";
  if (args.empty()) {
    json::Value r = json::Value::object();
    r.set("showing", overlay_ == Overlay::Panel);
    if (overlay_ == Overlay::Panel) r.set("id", panel_spec_.id).set("items", static_cast<int>(panel_spec_.items.size()));
    const panel::Area a = panel_area();  // where touches land on it, for bench automation
    json::Value area = json::Value::array();
    area.push(a.x).push(a.y).push(a.width).push(a.height);
    r.set("area", area);
    return "@panel " + r.dump();
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
