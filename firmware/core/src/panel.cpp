// Panel specs (hg/panel.hpp): parsing and limits. No LVGL here.
#include "hg/panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace hg::panel {
namespace {

// Cuts `s` to at most `max` bytes without splitting a UTF-8 character.
std::string clip(std::string_view s, size_t max) {
  if (s.size() <= max) return std::string(s);
  size_t n = max;
  while (n > 0 && (static_cast<uint8_t>(s[n]) & 0xC0) == 0x80) --n;
  return std::string(s.substr(0, n));
}

std::string number_text(double v) {
  char buf[32];
  if (std::fabs(v - std::round(v)) < 1e-9 && std::fabs(v) < 1e15) {
    std::snprintf(buf, sizeof buf, "%.0f", v);
  } else {
    std::snprintf(buf, sizeof buf, "%.2f", v);
    std::string s(buf);
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s;
  }
  return buf;
}

// A string, or a number written out; empty for anything else.
std::string text_of(const json::Value& v, size_t max = kMaxText) {
  if (v.is_string()) return clip(v.as_string(), max);
  if (v.is_number()) return number_text(v.as_number());
  return {};
}

std::string id_of(const json::Value& v) {
  if (v.is_number()) return number_text(v.as_number());
  return clip(v.str_or(""), kMaxId);
}

Tone tone_of(std::string_view s, Tone fallback) {
  if (s == "ok" || s == "good") return Tone::Ok;
  if (s == "warn" || s == "warning") return Tone::Warn;
  if (s == "bad" || s == "danger" || s == "error") return Tone::Bad;
  if (s == "accent" || s == "primary") return Tone::Accent;
  if (s == "neutral") return Tone::Neutral;
  return fallback;
}

const json::Value& entries_of(const json::Value& item) {
  const json::Value& items = item["items"];
  return items.is_array() ? items : item["rows"];
}

// Bars take a number as `value` and show `text`; the others show `value`.
void parse_entries(const json::Value& list, Kind kind, size_t max, std::vector<Entry>& out) {
  for (size_t i = 0; i < list.size() && out.size() < max; ++i) {
    const json::Value& e = list[i];
    if (!e.is_object()) continue;
    Entry entry;
    entry.id = id_of(e["id"]);
    entry.label = text_of(e["label"]);
    entry.detail = text_of(e["detail"], kMaxDetail);
    entry.tone = tone_of(e["state"].str_or(e["style"].str_or("")), Tone::Neutral);
    if (kind == Kind::Bars) {
      entry.amount = e["value"].as_number(0);
      if (!std::isfinite(entry.amount) || entry.amount < 0) entry.amount = 0;
      entry.value = e.has("text") ? text_of(e["text"]) : number_text(entry.amount);
    } else {
      entry.value = e.has("text") ? text_of(e["text"]) : text_of(e["value"]);
    }
    if (entry.label.empty() && entry.value.empty()) continue;
    if (kind == Kind::Buttons && (entry.label.empty() || entry.id.empty())) continue;  // a button needs both
    out.push_back(std::move(entry));
  }
}

bool parse_item(const json::Value& m, Item& item) {
  const std::string_view type = m["type"].str_or("");
  item.id = id_of(m["id"]);
  item.label = text_of(m["label"]);
  if (type == "text") {
    item.kind = Kind::Text;
    item.text = text_of(m["text"], kMaxDetail);
    const std::string_view style = m["style"].str_or("");
    item.style = style == "title" ? Item::Style::Title : style == "dim" ? Item::Style::Dim : Item::Style::Body;
    return !item.text.empty();
  }
  if (type == "stats" || type == "bars" || type == "list" || type == "buttons") {
    item.kind = type == "stats" ? Kind::Stats : type == "bars" ? Kind::Bars : type == "list" ? Kind::List : Kind::Buttons;
    parse_entries(entries_of(m), item.kind, item.kind == Kind::List ? kMaxListRows : kMaxEntries, item.entries);
    return !item.entries.empty();
  }
  if (type == "progress") {
    item.kind = Kind::Progress;
    item.value = m["value"].as_number(0);
    item.max = m["max"].as_number(100);
    if (!std::isfinite(item.value)) item.value = 0;
    if (!std::isfinite(item.max) || item.max <= 0) item.max = 100;
    item.text = m.has("text") ? text_of(m["text"]) : number_text(std::round(100.0 * item.value / item.max)) + "%";
    item.arc = m["style"].str_or("bar") == "arc";
    item.tone = tone_of(m["state"].str_or(""), Tone::Accent);
    return true;
  }
  if (type == "chart") {
    item.kind = Kind::Chart;
    item.line = m["style"].str_or("line") != "columns";
    const json::Value& values = m["values"];
    for (size_t i = 0; i < values.size(); ++i) {
      const double v = values[i].as_number(NAN);
      if (std::isfinite(v)) item.points.push_back(v);
    }
    if (item.points.size() > kMaxPoints)  // the latest ones, as the plugin keeps them
      item.points.erase(item.points.begin(), item.points.end() - static_cast<long>(kMaxPoints));
    const json::Value& labels = m["labels"];
    for (size_t i = 0; i < labels.size() && item.labels.size() < kMaxPoints; ++i)
      item.labels.push_back(text_of(labels[i], 24));
    return item.points.size() >= 2;
  }
  return false;
}

}  // namespace

bool parse(const json::Value& msg, Spec& out, std::string& error) {
  out = Spec();
  out.id = id_of(msg["id"]);
  if (out.id.empty()) {
    error = "panel without an id";
    return false;
  }
  out.title = text_of(msg["title"]);
  const double ttl = msg["ttl_s"].as_number(0);
  out.ttl_ms = std::isfinite(ttl) && ttl > 0 ? static_cast<uint32_t>(std::min(ttl, 86400.0) * 1000) : 0;
  const json::Value& items = msg["items"];
  for (size_t i = 0; i < items.size() && out.items.size() < kMaxItems; ++i) {
    if (!items[i].is_object()) continue;
    Item item;
    if (parse_item(items[i], item)) out.items.push_back(std::move(item));
  }
  if (out.items.empty()) {
    error = "panel with nothing to draw";
    return false;
  }
  return true;
}

#if !HG_PANEL
bool available() { return false; }
std::unique_ptr<View> make_view(const ViewConfig&) { return nullptr; }
#endif

}  // namespace hg::panel
