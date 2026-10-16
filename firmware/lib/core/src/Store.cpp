#include "Store.h"

#include <ArduinoJson.h>

#include "DateUtil.h"

namespace et {

namespace {

void writeItems(JsonArray arr, const std::vector<Item>& items, bool withDone) {
  for (auto& it : items) {
    JsonObject o = arr.add<JsonObject>();
    o["id"] = it.id;
    o["text"] = it.text;
    o["icon"] = it.icon;
    if (withDone) o["done"] = it.done;
  }
}

std::vector<Item> readItems(JsonArrayConst arr) {
  std::vector<Item> out;
  for (JsonObjectConst o : arr) {
    Item it;
    it.id = o["id"] | "";
    it.text = o["text"] | "";
    it.icon = o["icon"] | "star";
    it.done = o["done"] | false;
    if (!it.id.empty()) out.push_back(it);
  }
  return out;
}

bool readJson(Fs& fs, const char* p, JsonDocument& doc) {
  std::string raw;
  if (!fs.readAll(p, raw)) return false;
  return deserializeJson(doc, raw) == DeserializationError::Ok;
}

void writeJson(Fs& fs, const char* p, const JsonDocument& doc) {
  std::string out;
  serializeJson(doc, out);
  fs.writeAtomic(p, out);
}

}  // namespace

bool Store::load(Model& m) {
  JsonDocument doc;
  if (readJson(fs_, path::SETTINGS, doc)) {
    m.settings.resetHour = doc["resetHour"] | 4;
    m.settings.buzzer = doc["buzzer"] | true;
    m.settings.tzMin = doc["tzMin"] | 480;
  }
  bool known = false;
  doc.clear();
  if (readJson(fs_, path::TEMPLATES, doc)) {
    known = true;
    m.nextId = doc["nextId"] | 1;
    JsonArrayConst days = doc["days"];
    for (int d = 0; d < 7; ++d) m.templates[d] = readItems(days[d]);
  }
  doc.clear();
  if (readJson(fs_, path::OVERRIDES, doc)) {
    m.overrides.clear();
    for (JsonPairConst kv : doc.as<JsonObjectConst>()) {
      int day = isoToDay(kv.key().c_str());
      if (day == INT32_MIN) continue;
      DayPlan p;
      p.rest = kv.value()["rest"] | false;
      p.items = readItems(kv.value()["items"]);
      m.overrides[day] = p;
    }
  }
  doc.clear();
  if (readJson(fs_, path::TODAY, doc)) {
    int day = isoToDay(doc["day"] | "");
    if (day != INT32_MIN) {
      m.today.day = day;
      m.today.rest = doc["rest"] | false;
      m.today.custom = doc["custom"] | false;
      m.today.items = readItems(doc["items"]);
    }
  }
  doc.clear();
  if (readJson(fs_, path::STATS, doc)) {
    m.stats.stars = doc["stars"] | 0;
    m.stats.streak = doc["streak"] | 0;
    m.stats.history.clear();
    for (JsonObjectConst h : doc["history"].as<JsonArrayConst>()) {
      int day = isoToDay(h["day"] | "");
      if (day == INT32_MIN) continue;
      m.stats.history.push_back({day, DayStatus(h["s"] | 0), uint8_t(h["d"] | 0), uint8_t(h["t"] | 0)});
    }
  }
  return known;
}

void Store::saveSettings(const Model& m) {
  JsonDocument doc;
  doc["resetHour"] = m.settings.resetHour;
  doc["buzzer"] = m.settings.buzzer;
  doc["tzMin"] = m.settings.tzMin;
  writeJson(fs_, path::SETTINGS, doc);
}

void Store::saveTemplates(const Model& m) {
  JsonDocument doc;
  doc["nextId"] = m.nextId;
  JsonArray days = doc["days"].to<JsonArray>();
  for (auto& t : m.templates) writeItems(days.add<JsonArray>(), t, false);
  writeJson(fs_, path::TEMPLATES, doc);
}

void Store::saveOverrides(const Model& m) {
  JsonDocument doc;
  doc.to<JsonObject>();
  for (auto& kv : m.overrides) {
    JsonObject o = doc[dayToIso(kv.first)].to<JsonObject>();
    o["rest"] = kv.second.rest;
    writeItems(o["items"].to<JsonArray>(), kv.second.items, false);
  }
  writeJson(fs_, path::OVERRIDES, doc);
}

void Store::saveToday(const Model& m) {
  JsonDocument doc;
  doc["day"] = dayToIso(m.today.day);
  doc["rest"] = m.today.rest;
  doc["custom"] = m.today.custom;
  writeItems(doc["items"].to<JsonArray>(), m.today.items, true);
  writeJson(fs_, path::TODAY, doc);
}

void Store::saveStats(const Model& m) {
  JsonDocument doc;
  doc["stars"] = m.stats.stars;
  doc["streak"] = m.stats.streak;
  JsonArray h = doc["history"].to<JsonArray>();
  for (auto& e : m.stats.history) {
    JsonObject o = h.add<JsonObject>();
    o["day"] = dayToIso(e.day);
    o["s"] = int(e.status);
    o["d"] = e.done;
    o["t"] = e.total;
  }
  writeJson(fs_, path::STATS, doc);
}

void Store::saveAll(const Model& m) {
  saveSettings(m);
  saveTemplates(m);
  saveOverrides(m);
  saveToday(m);
  saveStats(m);
}

}  // namespace et
