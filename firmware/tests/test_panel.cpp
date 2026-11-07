// Panel specs (hg/panel.hpp): what parse() keeps, cuts and refuses. No LVGL needed.
#include <string>

#include "check.hpp"
#include "hg/json.hpp"
#include "hg/panel.hpp"

namespace {

hg::panel::Spec parsed(const std::string& text, bool* ok = nullptr, std::string* why = nullptr) {
  hg::json::Value v;
  hg::json::parse(text, v);
  hg::panel::Spec spec;
  std::string error;
  const bool good = hg::panel::parse(v, spec, error);
  if (ok) *ok = good;
  if (why) *why = error;
  return spec;
}

}  // namespace

TEST("panel: every item type parses, with its entries and defaults") {
  using hg::panel::Kind;
  using hg::panel::Tone;
  bool ok = false;
  const auto s = parsed(R"({"type":"panel","id":"p1","title":"Orçamento","ttl_s":30,"items":[
    {"type":"text","text":"Olá","style":"title"},
    {"type":"stats","items":[{"label":"Gasto","value":"R$ 3.401"},{"label":"N","value":7,"state":"ok"}]},
    {"type":"progress","label":"mês","value":25,"max":50,"style":"arc"},
    {"type":"bars","id":"cat","items":[{"id":"m","label":"Mercado","value":1200.5,"detail":"14 compras"},
                                      {"label":"Casa","value":460,"text":"R$ 460","state":"warn"}]},
    {"type":"chart","style":"columns","values":[1,2,"x",3],"labels":["a","b"]},
    {"type":"list","rows":[{"id":"media","label":"media","value":"85%","state":"bad"}]},
    {"type":"buttons","id":"act","items":[{"id":"go","label":"Ir","state":"primary"},{"label":"no id"}]}]})",
                        &ok);
  CHECK(ok);
  CHECK_EQ(s.id, std::string("p1"));
  CHECK_EQ(s.title, std::string("Orçamento"));
  CHECK_EQ(s.ttl_ms, 30000u);
  CHECK_EQ(s.items.size(), static_cast<size_t>(7));
  CHECK(s.items[0].kind == Kind::Text && s.items[0].style == hg::panel::Item::Style::Title);
  CHECK(s.items[1].kind == Kind::Stats && s.items[1].entries[1].value == "7" && s.items[1].entries[1].tone == Tone::Ok);
  CHECK(s.items[2].kind == Kind::Progress && s.items[2].arc && s.items[2].text == "50%");
  const auto& bars = s.items[3];
  CHECK(bars.kind == Kind::Bars && bars.id == "cat");
  CHECK(bars.entries[0].amount == 1200.5 && bars.entries[0].value == "1200.5" && bars.entries[0].tappable());
  CHECK(bars.entries[1].value == "R$ 460" && bars.entries[1].tone == Tone::Warn && !bars.entries[1].tappable());
  CHECK(s.items[4].kind == Kind::Chart && !s.items[4].line && s.items[4].points.size() == 3);  // "x" is not a number
  CHECK(s.items[5].kind == Kind::List && s.items[5].entries[0].tone == Tone::Bad);  // `rows` works as `items`
  CHECK(s.items[6].entries.size() == 1);  // a button needs an id and a label
}

TEST("panel: big panels are cut to the limits, not refused") {
  std::string items, bars;
  for (int i = 0; i < 20; ++i) items += std::string(i ? "," : "") + R"({"type":"text","text":"t"})";
  for (int i = 0; i < 12; ++i) bars += std::string(i ? "," : "") + R"({"label":"b","value":1})";
  bool ok = false;
  auto s = parsed(R"({"id":"p","items":[)" + items + "]}", &ok);
  CHECK(ok && s.items.size() == hg::panel::kMaxItems);
  s = parsed(R"({"id":"p","items":[{"type":"bars","items":[)" + bars + "]}]}", &ok);
  CHECK(ok && s.items[0].entries.size() == hg::panel::kMaxEntries);
  // A long label is cut without splitting a UTF-8 character.
  std::string long_label;
  for (int i = 0; i < 100; ++i) long_label += "é";
  s = parsed(R"({"id":"p","items":[{"type":"text","text":"x","label":")" + long_label + R"("}]})", &ok);
  CHECK(ok && s.items[0].label.size() <= hg::panel::kMaxText && s.items[0].label.size() % 2 == 0);
}

TEST("panel: a panel with no id or nothing to draw is refused") {
  bool ok = true;
  std::string why;
  parsed(R"({"items":[{"type":"text","text":"x"}]})", &ok, &why);
  CHECK(!ok && why == "panel without an id");
  parsed(R"({"id":"p","items":[{"type":"hologram"},{"type":"chart","values":[1]},{"type":"text"}]})", &ok, &why);
  CHECK(!ok && why == "panel with nothing to draw");
}
