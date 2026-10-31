#include "tessa_my_sigen_flow.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <string>

namespace esphome::tessa_my_sigen_flow {

using tessera::Font;
namespace ui = tessera::ui;

constexpr int SERIES = 5;
constexpr int MAX_POINTS = 300;  // 25 hours on the day clocks go back, sampled every five minutes
constexpr uint32_t SAMPLE_SECONDS = 300;
constexpr uint32_t CHUNK_SECONDS = 6 * 3600;

struct FlowData {
  tessera::List<float> values[SERIES];
  char entity[SERIES][64]{};
  char name[SERIES][40]{};
  char unit[SERIES][12]{};
  float current[SERIES]{};  // kW
  bool current_ok[SERIES]{};
  float live_sum[SERIES]{};
  uint16_t live_count[SERIES]{};
  int live_slot = -1;
  uint32_t day_start = 0, day_end = 0;
  int points = 0;
  uint32_t generation = 0;
  bool history_error = false;
  float price = 0;
  bool price_ok = false;

  FlowData() {
    for (auto &series : values) {
      series.resize(MAX_POINTS);
      for (int i = 0; i < MAX_POINTS; i++) series[i] = NAN;
    }
  }
};

static uint32_t local_midnight(uint32_t epoch) {
  uint32_t midnight = epoch;
  for (int i = 0; i < 3; i++) {
    const tessera::LocalTime t = tessera::local_time(midnight);
    const uint32_t elapsed = t.hour * 3600 + t.minute * 60 + t.second;
    if (!elapsed) break;
    midnight = midnight != epoch && elapsed >= 12 * 3600 ? midnight + 86400 - elapsed : midnight - elapsed;
  }
  return midnight;
}

static bool parse_number(JsonVariantConst v, float &out) {
  if (v.is<float>() || v.is<int>()) {
    out = v.as<float>();
    return std::isfinite(out);
  }
  const char *text = v | "";
  char *end = nullptr;
  out = std::strtof(text, &end);
  return end != text && *end == 0 && std::isfinite(out);
}

static float to_kw(float value, const char *unit) {
  if (std::strcmp(unit, "W") == 0 || std::strcmp(unit, "w") == 0) return value / 1000.0f;
  if (std::strcmp(unit, "MW") == 0) return value * 1000.0f;
  return value;
}

static void iso_time(uint32_t epoch, char out[24]) {
  const std::time_t value = static_cast<std::time_t>(epoch);
  std::tm utc{};
  gmtime_r(&value, &utc);
  std::strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &utc);
}

class TodayTile;

const FlowData &SigenFlow::data() const { return *flow_; }

void SigenFlow::configure(uint32_t epoch) {
  if (!flow_ || !epoch) return;
  const uint32_t start = local_midnight(epoch);
  const uint32_t end = local_midnight(start + 26 * 3600);
  const bool same_day = flow_->day_start == start && flow_->day_end == end;
  if (same_day) return;

  flow_->day_start = start;
  flow_->day_end = end;
  flow_->points = std::clamp<int>((end - start + SAMPLE_SECONDS - 1) / SAMPLE_SECONDS, 1, MAX_POINTS);
  flow_->history_error = false;
  flow_->live_slot = -1;
  for (int s = 0; s < SERIES; s++) { flow_->live_sum[s] = 0; flow_->live_count[s] = 0; }
  for (int s = 0; s < SERIES; s++) {
    for (int i = 0; i < MAX_POINTS; i++) flow_->values[s][i] = NAN;
  }
  request_id_ = 0;
  job_ = 0;
  const uint32_t complete_until = std::min<uint32_t>(end, epoch - epoch % SAMPLE_SECONDS);
  const uint8_t chunks = static_cast<uint8_t>((end - start + CHUNK_SECONDS - 1) / CHUNK_SECONDS);
  jobs_ = chunks * SERIES;
  history_until_ = complete_until;
  flow_->generation++;
  request_next();
}

void SigenFlow::update_sensor(uint8_t series, float value, const char *const entities[SERIES], uint32_t epoch) {
  if (!flow_ || series >= SERIES) return;
  bool entities_changed = false;
  for (int s = 0; s < SERIES; s++) {
    if (std::strcmp(flow_->entity[s], entities[s]) != 0) {
      std::snprintf(flow_->entity[s], sizeof(flow_->entity[s]), "%s", entities[s]);
      flow_->current_ok[s] = false;
      entities_changed = true;
    }
  }
  if (entities_changed) {
    flow_->day_start = 0;
    configure(epoch);
  } else {
    configure(epoch);
  }
  flow_->unit[series][0] = 'W';
  flow_->unit[series][1] = 0;
  flow_->current[series] = to_kw(value, flow_->unit[series]);
  flow_->current_ok[series] = std::isfinite(flow_->current[series]);
  flow_->name[series][0] = 0;
  flow_->generation++;
}

void SigenFlow::update_price(float value) {
  if (!flow_) return;
  flow_->price = value;
  flow_->price_ok = std::isfinite(value);
  flow_->generation++;
}

void SigenFlow::request_next() {
  if (!flow_ || request_id_ || !flow_->day_start || !jobs_) return;
  const uint8_t chunks = static_cast<uint8_t>((flow_->day_end - flow_->day_start + CHUNK_SECONDS - 1) / CHUNK_SECONDS);
  while (job_ < jobs_) {
    const uint8_t s = job_ / chunks;
    const uint8_t chunk = job_ % chunks;
    job_++;
    if (flow_->entity[s][0] == 0) continue;
    const uint32_t start = flow_->day_start + static_cast<uint32_t>(chunk) * CHUNK_SECONDS;
    const uint32_t end = std::min<uint32_t>(start + CHUNK_SECONDS, history_until_);
    if (start >= end) continue;
    request_start_ = start;
    request_end_ = end;
    request_series_ = s;
    JsonDocument request;
    request["ask"] = "recorder/statistics_during_period";
    JsonObject data = request["data"].to<JsonObject>();
    char from[24], to[24];
    iso_time(start, from);
    iso_time(end, to);
    data["start_time"] = from;
    data["end_time"] = to;
    data["period"] = "5minute";
    JsonArray ids = data["statistic_ids"].to<JsonArray>();
    ids.add(flow_->entity[s]);
    JsonArray types = data["types"].to<JsonArray>();
    types.add("mean");
    request_id_ = tessera::send(this, request.as<JsonObjectConst>());
    if (request_id_) return;
    flow_->history_error = true;
  }
  flow_->generation++;
}

void SigenFlow::on_message(JsonObjectConst message) {
  if (!request_id_ || (message["re"] | 0u) != request_id_) return;
  const bool ok = message["ok"] | false;
  if (ok) {
    JsonArrayConst timestamps = message["result"]["timestamps"].as<JsonArrayConst>();
    JsonArrayConst values = message["result"]["values"].as<JsonArrayConst>();
    const int count = std::min<int>(values.size(), timestamps.size());
    for (int i = 0; i < count; i++) {
      const double raw_epoch = timestamps[i].as<double>();
      const uint32_t epoch = static_cast<uint32_t>(raw_epoch > 10000000000.0 ? raw_epoch / 1000.0 : raw_epoch);
      if (epoch < flow_->day_start || epoch >= flow_->day_end) continue;
      const int slot = static_cast<int>((epoch - flow_->day_start) / SAMPLE_SECONDS);
      if (slot < 0 || slot >= flow_->points) continue;
      float value = 0;
      if (parse_number(values[i], value))
        flow_->values[request_series_][slot] = to_kw(value, flow_->unit[request_series_]);
    }
  } else {
    flow_->history_error = true;
  }
  flow_->generation++;
  request_id_ = 0;
  request_next();
}

bool SigenFlow::sample(uint32_t epoch) {
  if (!flow_ || !flow_->day_start || epoch < flow_->day_start || epoch >= flow_->day_end) return false;
  const int slot = static_cast<int>((epoch - flow_->day_start) / SAMPLE_SECONDS);
  if (slot < 0 || slot >= flow_->points) return false;
  if (slot != flow_->live_slot) {
    flow_->live_slot = slot;
    for (int s = 0; s < SERIES; s++) { flow_->live_sum[s] = 0; flow_->live_count[s] = 0; }
  }
  bool changed = false;
  for (int s = 0; s < SERIES; s++) {
    if (!flow_->current_ok[s]) continue;
    flow_->live_sum[s] += flow_->current[s];
    if (flow_->live_count[s] < UINT16_MAX) flow_->live_count[s]++;
    const float average = flow_->live_sum[s] / std::max<uint16_t>(1, flow_->live_count[s]);
    const float previous = flow_->values[s][slot];
    if (!std::isfinite(previous) || std::fabs(previous - average) > 0.0005f) {
      flow_->values[s][slot] = average;
      changed = true;
    }
  }
  return changed;
}

constexpr int NODE_SOLAR = 0, NODE_GRID = 1, NODE_HOME = 2, NODE_BATTERY = 3, NODE_CAR = 4;
constexpr uint32_t NODE_COLORS[SERIES] = {0xFF9D22, 0xA884E8, 0xFF9D22, 0xEC5B9A, 0x48D4D4};
constexpr uint32_t NODE_ICONS[SERIES] = {0xF0A72, 0xF0D3E, 0xF02DC, 0xF1901, 0xF0B6C};

class TodayTile : public tessera::Tile {
 public:
  explicit TodayTile(SigenFlow *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    plugin_->configure(tessera::epoch());
    price_box_ = lv_obj_create(c.parent);
    lv_obj_remove_style_all(price_box_);
    lv_obj_remove_flag(price_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(price_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(price_box_, width_ * 45 / 100, ui::line_height(Font::HEADLINE));
    lv_obj_set_pos(price_box_, 0, 0);
    lv_obj_set_style_pad_all(price_box_, 0, 0);
    lv_obj_set_style_radius(price_box_, ui::px(10), 0);
    lv_obj_set_style_bg_color(price_box_, lv_color_hex(0x2B261F), 0);
    lv_obj_set_style_bg_opa(price_box_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(price_box_, std::max(1, ui::px(1)), 0);
    lv_obj_set_style_border_color(price_box_, lv_color_hex(0x725127), 0);
    price_ = ui::label(price_box_, Font::BODY, theme::INK);
    lv_obj_remove_flag(price_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(price_, lv_color_hex(0xFFC56E), 0);
    lv_obj_set_size(price_, width_ * 45 / 100 - ui::px(12), ui::line_height(Font::HEADLINE));
    lv_obj_set_pos(price_, ui::px(6), 0);
    title_ = ui::label(c.parent, Font::HEADLINE, theme::INK);
    lv_obj_remove_flag(title_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(title_, width_ * 27 / 100, ui::line_height(Font::HEADLINE));
    lv_obj_set_pos(title_, width_ * 47 / 100, 0);

    const int header_h = ui::line_height(Font::HEADLINE);
    mode_pill_ = lv_obj_create(c.parent);
    lv_obj_remove_style_all(mode_pill_);
    lv_obj_remove_flag(mode_pill_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(mode_pill_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(mode_pill_, width_ / 4, header_h);
    lv_obj_set_pos(mode_pill_, width_ * 3 / 4, 0);
    lv_obj_set_style_pad_all(mode_pill_, 0, 0);
    lv_obj_set_style_bg_color(mode_pill_, lv_color_hex(0x397C68), 0);
    lv_obj_set_style_bg_opa(mode_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mode_pill_, 0, 0);
    lv_obj_set_style_radius(mode_pill_, header_h / 2, 0);
    mode_text_ = ui::label(mode_pill_, Font::BODY, theme::ON_ACCENT);
    lv_obj_remove_flag(mode_text_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mode_text_, width_ / 4, header_h);
    lv_obj_set_style_text_align(mode_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mode_text_, 0, 0);

    const int body_h = ui::line_height(Font::BODY);
    const int header_bottom = header_h + ui::px(8);
    const int footer_top = height_ - body_h - ui::px(2);
    const int outer_d = std::min({ui::px(88), width_ / 7, (footer_top - header_bottom) / 3});
    home_d_ = outer_d + ui::px(12);
    const int source_x = width_ * 32 / 100;
    const int grid_x = width_ * 13 / 100;
    const int right_x = width_ * 87 / 100;
    const int home_x = width_ * 56 / 100;
    const int top_y = header_bottom + body_h + ui::px(2) + outer_d / 2;
    const int bottom_y = footer_top - 2 * body_h - ui::px(4) - outer_d / 2;
    const int middle_y = (top_y + bottom_y) / 2;
    centers_[NODE_SOLAR] = {source_x, top_y};
    centers_[NODE_GRID] = {grid_x, middle_y};
    centers_[NODE_HOME] = {home_x, middle_y};
    centers_[NODE_BATTERY] = {source_x, bottom_y};
    centers_[NODE_CAR] = {right_x, middle_y};

    const int from_node[4] = {NODE_SOLAR, NODE_GRID, NODE_BATTERY, NODE_HOME};
    const int to_node[4] = {NODE_HOME, NODE_HOME, NODE_HOME, NODE_CAR};
    const int connector_colors[4] = {NODE_SOLAR, NODE_GRID, NODE_BATTERY, NODE_CAR};
    for (int i = 0; i < 4; i++) {
      connectors_[i] = lv_line_create(c.parent);
      lv_obj_remove_flag(connectors_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_line_width(connectors_[i], std::max(1, ui::px(2)), 0);
      lv_obj_set_style_line_color(connectors_[i], lv_color_hex(NODE_COLORS[connector_colors[i]]), 0);
      connector_points_[i][0] = {centers_[from_node[i]].x, centers_[from_node[i]].y};
      connector_points_[i][1] = {centers_[to_node[i]].x, centers_[to_node[i]].y};
      lv_line_set_points(connectors_[i], connector_points_[i], 2);
      dots_[i] = lv_obj_create(c.parent);
      lv_obj_remove_style_all(dots_[i]);
      lv_obj_remove_flag(dots_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(dots_[i], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_size(dots_[i], ui::px(8), ui::px(8));
      lv_obj_set_style_radius(dots_[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(dots_[i], 0, 0);
      lv_obj_set_style_bg_color(dots_[i], lv_color_hex(NODE_COLORS[connector_colors[i]]), 0);
      lv_obj_set_style_bg_opa(dots_[i], LV_OPA_COVER, 0);
    }

    const char *name_keys[SERIES] = {"solar", "grid", "home", "battery", "car"};
    const uint32_t badge_icons[SERIES] = {NODE_ICONS[0], NODE_ICONS[1], NODE_ICONS[2], NODE_ICONS[3], NODE_ICONS[4]};
    for (int n = 0; n < SERIES; n++) {
      const int d = n == NODE_HOME ? home_d_ : outer_d;
      circles_[n] = lv_obj_create(c.parent);
      lv_obj_remove_style_all(circles_[n]);
      lv_obj_remove_flag(circles_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(circles_[n], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_style_pad_all(circles_[n], 0, 0);
      lv_obj_set_size(circles_[n], d, d);
      lv_obj_set_pos(circles_[n], centers_[n].x - d / 2, centers_[n].y - d / 2);
      lv_obj_set_style_radius(circles_[n], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_bg_opa(circles_[n], LV_OPA_TRANSP, 0);
      lv_obj_set_style_border_width(circles_[n], std::max(2, ui::px(2)), 0);
      lv_obj_set_style_border_color(circles_[n], lv_color_hex(NODE_COLORS[n]), 0);
      icon_[n] = ui::label(circles_[n], Font::ICON, theme::INK);
      lv_obj_remove_flag(icon_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_align(icon_[n], LV_TEXT_ALIGN_CENTER, 0);
      ui::set_text(icon_[n], ui::icon(badge_icons[n]));
      const int icon_h = ui::line_height(Font::ICON);
      lv_obj_set_size(icon_[n], d - ui::px(12), icon_h);
      lv_obj_set_pos(icon_[n], ui::px(6), d / 2 - icon_h / 2 - ui::px(8));
      values_[n] = ui::label(circles_[n], Font::BODY, theme::INK);
      lv_obj_remove_flag(values_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_align(values_[n], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_size(values_[n], d - ui::px(10), 2 * body_h);
      lv_obj_set_pos(values_[n], ui::px(5), d / 2 + ui::px(2));
      names_[n] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_remove_flag(names_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_align(names_[n], LV_TEXT_ALIGN_CENTER, 0);
      const int label_w = n == NODE_HOME ? width_ * 24 / 100 : (n == NODE_GRID || n == NODE_CAR ? width_ * 24 / 100 : width_ * 26 / 100);
      lv_obj_set_size(names_[n], label_w, body_h);
      int label_y;
      if (n == NODE_BATTERY) label_y = centers_[n].y + d / 2 + ui::px(1);
      else label_y = centers_[n].y - d / 2 - body_h - ui::px(1);
      lv_obj_set_pos(names_[n], centers_[n].x - label_w / 2, label_y);
      ui::set_text(names_[n], plugin_->text(name_keys[n]));
    }
    status_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_remove_flag(status_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(status_, width_, body_h);
    lv_obj_set_pos(status_, 0, footer_top);
    day_seen_ = plugin_->data().day_start;
    refresh(tessera::epoch());
  }

  void on_tick(uint32_t epoch) override {
    plugin_->configure(epoch);
    if (day_seen_ != plugin_->data().day_start) {
      day_seen_ = plugin_->data().day_start;
    }
    plugin_->sample(epoch);
    refresh(epoch);
  }

  void on_theme() override { refresh(tessera::epoch()); }

  void on_tap() override {
    daily_ = !daily_;
    refresh(tessera::epoch());
  }

 private:
  void totals(float result[SERIES][2], uint32_t epoch) const {
    const FlowData &d = plugin_->data();
    for (int s = 0; s < SERIES; s++) result[s][0] = result[s][1] = 0;
    if (!d.day_start || !epoch) return;
    for (int s = 0; s < SERIES; s++) for (int i = 0; i < d.points; i++) {
      const float value = d.values[s][i];
      if (!std::isfinite(value)) continue;
      float seconds = SAMPLE_SECONDS;
      if (i == d.live_slot) {
        const uint32_t start = d.day_start + i * SAMPLE_SECONDS;
        seconds = epoch > start ? std::min<float>(SAMPLE_SECONDS, epoch - start) : 0;
      }
      const float kwh = value * seconds / 3600.0f;
      if (s == 2) result[s][value >= 0 ? 0 : 1] += std::fabs(kwh);  // grid import, export
      else if (s == 1) result[s][value >= 0 ? 1 : 0] += std::fabs(kwh);  // battery charge, discharge
      else result[s][0] += std::max(0.0f, kwh);
    }
  }

  void refresh(uint32_t epoch) {
    const FlowData &d = plugin_->data();
    ui::set_text(title_, plugin_->text("title"));
    ui::set_text(mode_text_, plugin_->text(daily_ ? "daily" : "live"));
    char price_text[48];
    if (d.price_ok) std::snprintf(price_text, sizeof(price_text), "%s %.2f SEK/kWh", plugin_->text("price"), d.price);
    else std::snprintf(price_text, sizeof(price_text), "%s -- SEK/kWh", plugin_->text("price"));
    ui::set_text(price_, price_text);
    const int map_series[SERIES] = {0, 2, 3, 1, 4};
    if (daily_) {
      float total[SERIES][2]{};
      totals(total, epoch);
      char text[SERIES][48];
      bool has_history = false;
      for (int s = 0; s < SERIES; s++)
        for (int i = 0; i < d.points; i++) has_history = has_history || std::isfinite(d.values[s][i]);
      if (!has_history) {
        for (auto &line : text) std::snprintf(line, sizeof(line), "--");
      } else {
        std::snprintf(text[NODE_SOLAR], sizeof(text[0]), "%.1f kWh", total[0][0]);
        std::snprintf(text[NODE_GRID], sizeof(text[0]), "IN %.1f\nOUT %.1f", total[2][0], total[2][1]);
        std::snprintf(text[NODE_HOME], sizeof(text[0]), "%.1f kWh", total[3][0]);
        std::snprintf(text[NODE_BATTERY], sizeof(text[0]), "CHG %.1f\nDIS %.1f", total[1][0], total[1][1]);
        std::snprintf(text[NODE_CAR], sizeof(text[0]), "%.1f kWh", total[4][0]);
      }
      for (int n = 0; n < SERIES; n++) ui::set_text(values_[n], text[n]);
      if (d.history_error) ui::set_text(status_, plugin_->text("no_history"));
      else if (!has_history) ui::set_text(status_, plugin_->text("loading_history"));
      else ui::set_text(status_, plugin_->text("daily_hint"));
    } else {
      for (int n = 0; n < SERIES; n++) {
        const int s = map_series[n];
        char text[48];
        if (!d.current_ok[s]) std::snprintf(text, sizeof(text), "--");
        else if (s == 2) std::snprintf(text, sizeof(text), "%s\n%.2f kW", d.current[s] < 0 ? "OUT" : "IN", std::fabs(d.current[s]));
        else if (s == 1) std::snprintf(text, sizeof(text), "%s\n%.2f kW", d.current[s] < 0 ? "CHG" : "DIS", std::fabs(d.current[s]));
        else std::snprintf(text, sizeof(text), "%.2f kW", std::max(0.0f, d.current[s]));
        ui::set_text(values_[n], text);
      }
      if (d.history_error) ui::set_text(status_, plugin_->text("no_history"));
      else ui::set_text(status_, plugin_->text("live_hint"));
    }
    update_flows(epoch);
  }

  void update_flows(uint32_t epoch) {
    const FlowData &d = plugin_->data();
    const float values[4] = {d.current[0], d.current[2], d.current[1], d.current[4]};
    const bool valid[4] = {d.current_ok[0], d.current_ok[2], d.current_ok[1], d.current_ok[4]};
    const int source[4] = {NODE_SOLAR, NODE_GRID, NODE_BATTERY, NODE_HOME};
    const int dest[4] = {NODE_HOME, NODE_HOME, NODE_HOME, NODE_CAR};
    const bool active[4] = {valid[0] && values[0] > 0.01f, valid[1] && std::fabs(values[1]) > 0.01f,
                            valid[2] && std::fabs(values[2]) > 0.01f, valid[3] && values[3] > 0.01f};
    for (int i = 0; i < 4; i++) {
      const int x0 = centers_[source[i]].x, y0 = centers_[source[i]].y;
      const int x1 = centers_[dest[i]].x, y1 = centers_[dest[i]].y;
      const bool reverse = (i == 1 && values[i] < 0) || (i == 2 && values[i] < 0);
      const float phase = std::fmod((epoch % 4) / 4.0f + i * 0.19f, 1.0f);
      const float p = reverse ? 1.0f - phase : phase;
      lv_obj_set_pos(dots_[i], static_cast<int>(x0 + (x1 - x0) * p) - ui::px(4),
                     static_cast<int>(y0 + (y1 - y0) * p) - ui::px(4));
      const uint32_t color = i == 1 && values[i] < 0 ? 0xEA788F : i == 2 && values[i] < 0 ? 0x43D9D0
                               : NODE_COLORS[i == 0 ? NODE_SOLAR : i == 1 ? NODE_GRID : i == 2 ? NODE_BATTERY : NODE_CAR];
      lv_obj_set_style_line_color(connectors_[i], lv_color_hex(color), 0);
      lv_obj_set_style_bg_color(dots_[i], lv_color_hex(color), 0);
      if (!daily_ && active[i]) lv_obj_clear_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  SigenFlow *plugin_;
  lv_obj_t *title_{}, *price_box_{}, *price_{}, *mode_pill_{}, *mode_text_{}, *status_{};
  lv_obj_t *circles_[SERIES]{}, *icon_[SERIES]{}, *names_[SERIES]{}, *values_[SERIES]{};
  lv_obj_t *connectors_[SERIES]{}, *dots_[SERIES]{};
  lv_point_precise_t connector_points_[SERIES][2]{};
  tessera::Point centers_[SERIES]{};
  int width_ = 0, height_ = 0, home_d_ = 0;
  uint32_t day_seen_ = 0;
  bool daily_ = false;
};

void SigenFlow::setup() {
  flow_ = new FlowData();
  add_tile("today", [this]() { return new TodayTile(this); });
}

}  // namespace esphome::tessa_my_sigen_flow
