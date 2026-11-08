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

constexpr int SERIES = 6;
constexpr int PRICE_SERIES = SERIES;
constexpr int HISTORY_SERIES = SERIES + 1;
constexpr int FLOWS = 8;
constexpr int MAX_POINTS = 300;  // 25 hours on the day clocks go back, sampled every five minutes
constexpr uint32_t SAMPLE_SECONDS = 300;
constexpr uint32_t CHUNK_SECONDS = 6 * 3600;

struct FlowData {
  tessera::List<float> values[SERIES];
  tessera::List<float> price_values;
  char entity[HISTORY_SERIES][64]{};
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
  float battery_soc = 0;
  bool battery_soc_ok = false;

  FlowData() {
    for (int i = 0; i < SERIES; i++) std::snprintf(unit[i], sizeof(unit[i]), "W");
    for (auto &series : values) {
      series.resize(MAX_POINTS);
      for (int i = 0; i < MAX_POINTS; i++) series[i] = NAN;
    }
    price_values.resize(MAX_POINTS);
    for (int i = 0; i < MAX_POINTS; i++) price_values[i] = NAN;
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

static float kw_factor(const char *unit) {
  if (std::strcmp(unit, "W") == 0 || std::strcmp(unit, "w") == 0) return 0.001f;
  if (std::strcmp(unit, "MW") == 0) return 1000.0f;
  return 1.0f;  // kW is the Home Assistant default when a unit is not provided
}

static float to_kw(float value, const char *unit) {
  return value * kw_factor(unit);
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
  for (int i = 0; i < MAX_POINTS; i++) flow_->price_values[i] = NAN;
  request_id_ = 0;
  job_ = 0;
  const uint32_t complete_until = std::min<uint32_t>(end, epoch - epoch % SAMPLE_SECONDS);
  const uint8_t chunks = static_cast<uint8_t>((end - start + CHUNK_SECONDS - 1) / CHUNK_SECONDS);
  jobs_ = chunks * HISTORY_SERIES;
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
  flow_->current[series] = to_kw(value, flow_->unit[series]);
  flow_->current_ok[series] = std::isfinite(flow_->current[series]);
  flow_->name[series][0] = 0;
  flow_->generation++;
}

void SigenFlow::update_unit(uint8_t series, const char *unit) {
  if (!flow_ || series >= SERIES || !unit || !unit[0] || std::strcmp(flow_->unit[series], unit) == 0) return;
  const float old_factor = kw_factor(flow_->unit[series]);
  const float ratio = kw_factor(unit) / old_factor;
  std::snprintf(flow_->unit[series], sizeof(flow_->unit[series]), "%s", unit);
  flow_->current[series] *= ratio;
  flow_->live_sum[series] *= ratio;
  for (int i = 0; i < flow_->points; i++)
    if (std::isfinite(flow_->values[series][i])) flow_->values[series][i] *= ratio;
  flow_->generation++;
}

void SigenFlow::update_price(float value, const char *entity, uint32_t epoch) {
  if (!flow_) return;
  if (entity && std::strcmp(flow_->entity[PRICE_SERIES], entity) != 0) {
    std::snprintf(flow_->entity[PRICE_SERIES], sizeof(flow_->entity[PRICE_SERIES]), "%s", entity);
    flow_->day_start = 0;
    configure(epoch);
  } else {
    configure(epoch);
  }
  flow_->price = value;
  flow_->price_ok = std::isfinite(value);
  flow_->generation++;
}

void SigenFlow::update_battery_soc(float value) {
  if (!flow_) return;
  flow_->battery_soc = value;
  flow_->battery_soc_ok = std::isfinite(value);
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
      if (request_series_ == PRICE_SERIES) flow_->price_values[slot] = value;
      else flow_->values[request_series_][slot] = to_kw(value, flow_->unit[request_series_]);
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

constexpr int NODE_SOLAR = 0, NODE_GRID = 1, NODE_HOME = 2, NODE_BATTERY = 3, NODE_CAR = 4, NODE_HEAT_PUMP = 5;
constexpr uint32_t NODE_COLORS[SERIES] = {0xF2A23A, 0xA884E8, 0xF2A23A, 0xE9759D, 0x52C7C5, 0xB4AA58};
constexpr uint32_t NODE_ICONS[SERIES] = {0xF0A72, 0xF0D3E, 0xF02DC, 0xF1901, 0xF0B6C, 0xF0438};
constexpr uint32_t FLOW_COLORS[FLOWS] = {0xF2A23A, 0xE9759D, 0xA884E8, 0xA884E8, 0xE9759D, 0x52C7C5, 0x62B8D8, 0xB4AA58};

class TodayTile : public tessera::Tile {
 public:
  explicit TodayTile(SigenFlow *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    compact_layout_ = width_ < ui::px(560) || height_ < ui::px(300);
    plugin_->configure(tessera::epoch());
    price_ = ui::label(c.parent, compact_layout_ ? Font::BODY : Font::HEADLINE, theme::INK);
    lv_obj_remove_flag(price_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_color(price_, theme::color(theme::INK), 0);
    lv_obj_set_size(price_, width_ * (compact_layout_ ? 64 : 68) / 100, ui::line_height(Font::HEADLINE));
    lv_obj_set_pos(price_, 0, 0);
    const Font title_font = width_ < ui::px(500) ? Font::BODY : Font::HEADLINE;
    title_ = ui::label(c.parent, title_font, theme::INK);
    lv_obj_remove_flag(title_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(title_, width_ * 27 / 100, ui::line_height(Font::HEADLINE));
    lv_obj_set_pos(title_, width_ * 47 / 100, 0);
    lv_obj_add_flag(title_, LV_OBJ_FLAG_HIDDEN);

    const int header_h = ui::line_height(Font::HEADLINE);
    const int mode_w = width_ * (compact_layout_ ? 35 : 29) / 100;
    mode_h_ = header_h + ui::px(8);
    mode_pill_ = lv_obj_create(c.parent);
    lv_obj_remove_style_all(mode_pill_);
    lv_obj_remove_flag(mode_pill_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(mode_pill_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mode_pill_, mode_w, mode_h_);
    lv_obj_set_pos(mode_pill_, width_ - mode_w, -ui::px(2));
    lv_obj_set_style_pad_all(mode_pill_, 0, 0);
    lv_obj_set_style_bg_color(mode_pill_, theme::color(theme::ACCENT), 0);
    lv_obj_set_style_bg_color(mode_pill_, theme::color(theme::ACCENT_PRESSED), LV_STATE_PRESSED);
    lv_obj_set_style_bg_opa(mode_pill_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(mode_pill_, 0, 0);
    lv_obj_set_style_radius(mode_pill_, mode_h_ / 2, 0);
    mode_text_ = ui::label(mode_pill_, Font::BODY, theme::ON_ACCENT);
    lv_obj_remove_flag(mode_text_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(mode_text_, mode_w, mode_h_);
    lv_obj_set_style_text_align(mode_text_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(mode_text_, 0, 0);
    lv_obj_add_event_cb(mode_pill_, [](lv_event_t *event) {
      auto *tile = static_cast<TodayTile *>(lv_event_get_user_data(event));
      tile->toggle_mode();
    }, LV_EVENT_CLICKED, this);

    const int body_h = ui::line_height(Font::BODY);
    const int header_bottom = header_h + ui::px(8);
    const int footer_top = height_ - body_h - ui::px(2);
    if (compact_layout_) {
      // Keep compact tiles readable: use two wider columns on a 1-column tile,
      // three columns on a medium tile, and all five metrics when there is room.
      const int cols = width_ < ui::px(280) ? 2 : (width_ < ui::px(520) ? 3 : 5);
      const int rows = (SERIES + cols - 1) / cols;
      const int card_top = header_bottom;
      const bool tiny_height = height_ < ui::px(160);
      const int card_bottom = tiny_height ? footer_top : footer_top - body_h - ui::px(2);
      const int row_h = std::max(body_h, (card_bottom - card_top) / rows);
      const int gap = width_ < ui::px(280) ? ui::px(1) : ui::px(3);
      for (int n = 0; n < SERIES; n++) {
        const int col = n % cols;
        const int row = n / cols;
        const int cell_w = width_ / cols;
        const int card_w = cell_w - gap;
        const int card_h = std::max(body_h, row_h - gap);
        compact_cards_[n] = lv_obj_create(c.parent);
        lv_obj_remove_style_all(compact_cards_[n]);
        lv_obj_remove_flag(compact_cards_[n], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(compact_cards_[n], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(compact_cards_[n], card_w, card_h);
        lv_obj_set_pos(compact_cards_[n], col * cell_w + gap / 2, card_top + row * row_h);
        lv_obj_set_style_bg_color(compact_cards_[n], theme::color(theme::CARD), 0);
        lv_obj_set_style_bg_opa(compact_cards_[n], LV_OPA_COVER, 0);
        lv_obj_set_style_border_color(compact_cards_[n], lv_color_hex(NODE_COLORS[n]), 0);
        lv_obj_set_style_border_width(compact_cards_[n], std::max(1, ui::px(1)), 0);
        lv_obj_set_style_radius(compact_cards_[n], ui::px(6), 0);
        compact_values_[n] = ui::label(compact_cards_[n], Font::BODY, theme::INK);
        lv_obj_remove_flag(compact_values_[n], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_align(compact_values_[n], LV_TEXT_ALIGN_CENTER, 0);
        ui::set_color(compact_values_[n], theme::INK);
        lv_obj_set_size(compact_values_[n], card_w - gap, card_h);
        lv_obj_set_pos(compact_values_[n], gap / 2, 0);
      }
      if (!tiny_height) {
        compact_status_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
        lv_obj_remove_flag(compact_status_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_size(compact_status_, width_, body_h);
        lv_obj_set_style_text_align(compact_status_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(compact_status_, 0, footer_top);
      }
      day_seen_ = plugin_->data().day_start;
      refresh(tessera::epoch());
      return;
    }
    const int outer_d = std::min({ui::px(96), width_ / 6, (footer_top - header_bottom) / 3});
    compact_nodes_ = outer_d < ui::px(76);
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
    centers_[NODE_CAR] = {right_x, bottom_y};
    centers_[NODE_HEAT_PUMP] = {right_x, top_y};

    const int from_node[FLOWS] = {NODE_SOLAR, NODE_SOLAR, NODE_GRID, NODE_GRID, NODE_BATTERY, NODE_HOME, NODE_HOME, NODE_HOME};
    const int to_node[FLOWS] = {NODE_HOME, NODE_BATTERY, NODE_HOME, NODE_BATTERY, NODE_HOME, NODE_CAR, NODE_GRID, NODE_HEAT_PUMP};
    for (int i = 0; i < FLOWS; i++) {
      connectors_[i] = lv_line_create(c.parent);
      lv_obj_remove_flag(connectors_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_line_width(connectors_[i], std::max(1, ui::px(2)), 0);
      lv_obj_set_style_line_color(connectors_[i], lv_color_hex(FLOW_COLORS[i]), 0);
      const float dx = static_cast<float>(centers_[to_node[i]].x - centers_[from_node[i]].x);
      const float dy = static_cast<float>(centers_[to_node[i]].y - centers_[from_node[i]].y);
      const float length = std::max(1.0f, std::sqrt(dx * dx + dy * dy));
      const float ux = dx / length, uy = dy / length;
      const float from_radius = (from_node[i] == NODE_HOME ? home_d_ : outer_d) / 2.0f;
      const float to_radius = (to_node[i] == NODE_HOME ? home_d_ : outer_d) / 2.0f;
      connector_points_[i][0] = {
          static_cast<decltype(connector_points_[i][0].x)>(std::lround(centers_[from_node[i]].x + ux * from_radius)),
          static_cast<decltype(connector_points_[i][0].y)>(std::lround(centers_[from_node[i]].y + uy * from_radius))};
      connector_points_[i][1] = {
          static_cast<decltype(connector_points_[i][1].x)>(std::lround(centers_[to_node[i]].x - ux * to_radius)),
          static_cast<decltype(connector_points_[i][1].y)>(std::lround(centers_[to_node[i]].y - uy * to_radius))};
      lv_line_set_points(connectors_[i], connector_points_[i], 2);
      dots_[i] = lv_obj_create(c.parent);
      lv_obj_remove_style_all(dots_[i]);
      lv_obj_remove_flag(dots_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(dots_[i], LV_OBJ_FLAG_SCROLLABLE);
      lv_obj_set_size(dots_[i], ui::px(8), ui::px(8));
      lv_obj_set_style_radius(dots_[i], LV_RADIUS_CIRCLE, 0);
      lv_obj_set_style_border_width(dots_[i], 0, 0);
      lv_obj_set_style_bg_color(dots_[i], lv_color_hex(FLOW_COLORS[i]), 0);
      lv_obj_set_style_bg_opa(dots_[i], LV_OPA_COVER, 0);
    }

    const char *name_keys[SERIES] = {"solar", "grid", "home", "battery", "car", "heat_pump"};
    const uint32_t badge_icons[SERIES] = {NODE_ICONS[0], NODE_ICONS[1], NODE_ICONS[2], NODE_ICONS[3], NODE_ICONS[4], NODE_ICONS[5]};
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
      lv_obj_set_style_bg_color(circles_[n], theme::color(theme::CARD), 0);
      lv_obj_set_style_bg_opa(circles_[n], LV_OPA_COVER, 0);
      lv_obj_set_style_border_width(circles_[n], std::max(2, ui::px(2)), 0);
      lv_obj_set_style_border_color(circles_[n], lv_color_hex(NODE_COLORS[n]), 0);
      icon_[n] = ui::label(circles_[n], Font::ICON, theme::INK);
      lv_obj_remove_flag(icon_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_align(icon_[n], LV_TEXT_ALIGN_CENTER, 0);
      ui::set_text(icon_[n], ui::icon(badge_icons[n]));
      const int icon_h = ui::line_height(Font::ICON);
      lv_obj_set_size(icon_[n], d - ui::px(12), icon_h);
      if (n == NODE_BATTERY) {
        lv_obj_set_pos(icon_[n], ui::px(6), body_h + ui::px(1));
        soc_ = ui::label(circles_[n], Font::BODY, theme::INK);
        lv_obj_remove_flag(soc_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_align(soc_, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(soc_, d - ui::px(10), body_h);
        lv_obj_set_pos(soc_, ui::px(5), ui::px(1));
      } else if (n == NODE_GRID) {
        lv_obj_set_pos(icon_[n], ui::px(6), ui::px(7));
      } else {
        lv_obj_set_pos(icon_[n], ui::px(6), d / 4 - icon_h / 2);
      }
      if (compact_nodes_) lv_obj_add_flag(icon_[n], LV_OBJ_FLAG_HIDDEN);
      values_[n] = ui::label(circles_[n], Font::BODY, theme::INK);
      lv_obj_remove_flag(values_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_align(values_[n], LV_TEXT_ALIGN_CENTER, 0);
      lv_obj_set_size(values_[n], d - ui::px(10), body_h);
      const bool directional = n == NODE_GRID || n == NODE_BATTERY;
      const int value_y = directional ? d - 2 * body_h - ui::px(2) : d / 2 + ui::px(1);
      lv_obj_set_pos(values_[n], ui::px(5), value_y);
      if (directional) {
        secondary_[n] = ui::label(circles_[n], Font::BODY, theme::INK);
        lv_obj_remove_flag(secondary_[n], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_text_align(secondary_[n], LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_size(secondary_[n], d - ui::px(10), body_h);
        lv_obj_set_pos(secondary_[n], ui::px(5), value_y + body_h);
      }
      if (n == NODE_GRID) {
        ui::set_color(values_[n], theme::INK);
        ui::set_color(secondary_[n], theme::MUTED);
      } else if (n == NODE_BATTERY) {
        ui::set_color(values_[n], theme::INK);
        ui::set_color(secondary_[n], theme::MUTED);
      }
      names_[n] = ui::label(c.parent, Font::BODY, theme::MUTED);
      lv_obj_remove_flag(names_[n], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_align(names_[n], LV_TEXT_ALIGN_CENTER, 0);
      const int label_w = n == NODE_HOME ? width_ * 24 / 100 : (n == NODE_GRID || n == NODE_CAR || n == NODE_HEAT_PUMP ? width_ * 24 / 100 : width_ * 26 / 100);
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

 private:
  void toggle_mode() {
    daily_ = !daily_;
    refresh(tessera::epoch());
  }

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
      else if (s == 1) result[s][value >= 0 ? 0 : 1] += std::fabs(kwh);  // battery charge, discharge
      else result[s][0] += std::max(0.0f, kwh);
    }
  }

  bool average_price_today(float &average, uint32_t epoch) const {
    const FlowData &d = plugin_->data();
    if (!d.day_start || epoch <= d.day_start) return false;
    const int current_slot = std::clamp<int>((epoch - d.day_start) / SAMPLE_SECONDS, 0, d.points - 1);
    float sum = 0;
    int count = 0;
    for (int i = 0; i < current_slot; i++) {
      if (!std::isfinite(d.price_values[i])) continue;
      sum += d.price_values[i];
      count++;
    }
    if (!count) return false;
    average = sum / count;
    return true;
  }

  void refresh(uint32_t epoch) {
    const FlowData &d = plugin_->data();
    ui::set_text(title_, plugin_->text("title"));
    ui::set_color(price_, theme::INK);
    lv_obj_set_style_bg_color(mode_pill_, theme::color(theme::ACCENT), 0);
    lv_obj_set_style_bg_color(mode_pill_, theme::color(theme::ACCENT_PRESSED), LV_STATE_PRESSED);
    ui::set_color(mode_text_, theme::ON_ACCENT);
    if (status_) ui::set_color(status_, theme::SUBTLE);
    if (compact_status_) ui::set_color(compact_status_, theme::SUBTLE);
    for (int n = 0; n < SERIES; n++) {
      if (circles_[n]) lv_obj_set_style_bg_color(circles_[n], theme::color(theme::CARD), 0);
      if (compact_cards_[n]) lv_obj_set_style_bg_color(compact_cards_[n], theme::color(theme::CARD), 0);
      if (icon_[n]) ui::set_color(icon_[n], theme::INK);
      if (names_[n]) ui::set_color(names_[n], theme::MUTED);
      if (values_[n]) ui::set_color(values_[n], theme::INK);
      if (secondary_[n]) ui::set_color(secondary_[n], theme::MUTED);
      if (compact_values_[n]) ui::set_color(compact_values_[n], theme::INK);
    }
    if (soc_) ui::set_color(soc_, theme::INK);
    ui::set_text(mode_text_, plugin_->text(compact_layout_ ? (daily_ ? "daily_compact" : "live_compact") : (daily_ ? "daily" : "live")));
    char price_text[48];
    if (daily_) {
      float average = 0;
      if (average_price_today(average, epoch))
        std::snprintf(price_text, sizeof(price_text), compact_layout_ ? "AVG %.2f SEK/kWh" : "AVERAGE TODAY  %.2f SEK/kWh", average);
      else
        std::snprintf(price_text, sizeof(price_text), compact_layout_ ? "AVG -- SEK/kWh" : "AVERAGE TODAY  -- SEK/kWh");
    } else if (d.price_ok) {
      std::snprintf(price_text, sizeof(price_text), compact_layout_ ? "NOW %.2f SEK/kWh" : "PRICE RIGHT NOW  %.2f SEK/kWh", d.price);
    } else {
      std::snprintf(price_text, sizeof(price_text), compact_layout_ ? "NOW -- SEK/kWh" : "PRICE RIGHT NOW  -- SEK/kWh");
    }
    ui::set_text(price_, price_text);
    const int map_series[SERIES] = {0, 2, 3, 1, 4, 5};
    if (compact_layout_) {
      const bool tiny_width = width_ < ui::px(280);
      if (daily_) {
        float total[SERIES][2]{};
        totals(total, epoch);
        bool has_history = false;
        for (int s = 0; s < SERIES; s++)
          for (int i = 0; i < d.points; i++) has_history = has_history || std::isfinite(d.values[s][i]);
        if (!has_history) {
          for (int n = 0; n < SERIES; n++) ui::set_text(compact_values_[n], "--");
          char battery_text[32];
          if (d.battery_soc_ok) std::snprintf(battery_text, sizeof(battery_text), "%.0f%% C-- D--", d.battery_soc);
          else std::snprintf(battery_text, sizeof(battery_text), "--%% C-- D--");
          ui::set_text(compact_values_[NODE_BATTERY], battery_text);
        } else {
          char text[48];
          std::snprintf(text, sizeof(text), "S %.1f", total[0][0]);
          ui::set_text(compact_values_[NODE_SOLAR], text);
          std::snprintf(text, sizeof(text), tiny_width ? "I%.1f/O%.1f" : "IN %.1f OUT %.1f", total[2][0], total[2][1]);
          ui::set_text(compact_values_[NODE_GRID], text);
          std::snprintf(text, sizeof(text), "H %.1f", total[3][0]);
          ui::set_text(compact_values_[NODE_HOME], text);
          if (d.battery_soc_ok) std::snprintf(text, sizeof(text), tiny_width ? "%.0f%% C%.1f D%.1f" : "%.0f%% C%.1f D%.1f", d.battery_soc, total[1][0], total[1][1]);
          else std::snprintf(text, sizeof(text), "--%% C%.1f D%.1f", total[1][0], total[1][1]);
          ui::set_text(compact_values_[NODE_BATTERY], text);
          std::snprintf(text, sizeof(text), tiny_width ? "E %.1f" : "EV %.1f", total[4][0]);
          ui::set_text(compact_values_[NODE_CAR], text);
          std::snprintf(text, sizeof(text), "HP %.1f", total[5][0]);
          ui::set_text(compact_values_[NODE_HEAT_PUMP], text);
        }
        if (compact_status_) {
          if (d.history_error) ui::set_text(compact_status_, plugin_->text("no_history"));
          else ui::set_text(compact_status_, plugin_->text("compact_daily_status"));
        }
      } else {
        const float grid = d.current_ok[2] ? d.current[2] : 0;
        const float battery = d.current_ok[1] ? d.current[1] : 0;
        for (int n = 0; n < SERIES; n++) {
          const int s = map_series[n];
          char text[48];
          if (!d.current_ok[s]) std::snprintf(text, sizeof(text), "--");
          else if (n == NODE_SOLAR) std::snprintf(text, sizeof(text), "S %.1f", std::max(0.0f, d.current[s]));
          else if (n == NODE_GRID) std::snprintf(text, sizeof(text), tiny_width ? "I%.1f/O%.1f" : "IN %.1f OUT %.1f", std::max(0.0f, grid), std::max(0.0f, -grid));
          else if (n == NODE_HOME) std::snprintf(text, sizeof(text), "H %.1f", std::max(0.0f, d.current[s]));
          else if (n == NODE_BATTERY && d.battery_soc_ok) std::snprintf(text, sizeof(text), "%.0f%% C%.1f D%.1f", d.battery_soc, std::max(0.0f, battery), std::max(0.0f, -battery));
          else if (n == NODE_BATTERY) std::snprintf(text, sizeof(text), "--%% C%.1f D%.1f", std::max(0.0f, battery), std::max(0.0f, -battery));
          else if (n == NODE_HEAT_PUMP) std::snprintf(text, sizeof(text), "HP %.1f", std::max(0.0f, d.current[s]));
          else std::snprintf(text, sizeof(text), tiny_width ? "E %.1f" : "EV %.1f", std::max(0.0f, d.current[s]));
          ui::set_text(compact_values_[n], text);
        }
        if (compact_status_) {
          if (d.history_error) ui::set_text(compact_status_, plugin_->text("no_history"));
          else ui::set_text(compact_status_, plugin_->text("compact_live_status"));
        }
      }
      return;
    }
    if (daily_) {
      float total[SERIES][2]{};
      totals(total, epoch);
      char text[SERIES][48];
      bool has_history = false;
      for (int s = 0; s < SERIES; s++)
        for (int i = 0; i < d.points; i++) has_history = has_history || std::isfinite(d.values[s][i]);
      if (!has_history) {
        for (auto &line : text) std::snprintf(line, sizeof(line), "--");
        ui::set_text(secondary_[NODE_GRID], "--");
        ui::set_text(secondary_[NODE_BATTERY], "--");
      } else {
        std::snprintf(text[NODE_SOLAR], sizeof(text[0]), "%.1f kWh", total[0][0]);
        std::snprintf(text[NODE_GRID], sizeof(text[0]), "IN %.1f", total[2][0]);
        std::snprintf(text[NODE_HOME], sizeof(text[0]), "%.1f kWh", total[3][0]);
        std::snprintf(text[NODE_BATTERY], sizeof(text[0]), "CHG %.1f", total[1][0]);
        std::snprintf(text[NODE_CAR], sizeof(text[0]), "%.1f kWh", total[4][0]);
        std::snprintf(text[NODE_HEAT_PUMP], sizeof(text[0]), "%.1f kWh", total[5][0]);
        std::snprintf(price_text, sizeof(price_text), "OUT %.1f", total[2][1]);
        ui::set_text(secondary_[NODE_GRID], price_text);
        std::snprintf(price_text, sizeof(price_text), "DIS %.1f", total[1][1]);
        ui::set_text(secondary_[NODE_BATTERY], price_text);
      }
      for (int n = 0; n < SERIES; n++) ui::set_text(values_[n], text[n]);
      if (d.battery_soc_ok) std::snprintf(price_text, sizeof(price_text), "%.0f%%", d.battery_soc);
      else std::snprintf(price_text, sizeof(price_text), "--%%");
      ui::set_text(soc_, price_text);
      if (d.history_error) ui::set_text(status_, plugin_->text("no_history"));
      else if (!has_history) ui::set_text(status_, plugin_->text("loading_history"));
      else ui::set_text(status_, plugin_->text(compact_nodes_ ? "daily_hint_compact" : "daily_hint"));
    } else {
      for (int n = 0; n < SERIES; n++) {
        const int s = map_series[n];
        char text[48];
        if (!d.current_ok[s]) std::snprintf(text, sizeof(text), "--");
        else if (s == 2) std::snprintf(text, sizeof(text), "IN %.2f", std::max(0.0f, d.current[s]));
        else if (s == 1) std::snprintf(text, sizeof(text), "CHG %.2f", std::max(0.0f, d.current[s]));
        else std::snprintf(text, sizeof(text), "%.2f kW", std::max(0.0f, d.current[s]));
        ui::set_text(values_[n], text);
        if (n == NODE_GRID) {
          char out[32];
          if (!d.current_ok[s]) std::snprintf(out, sizeof(out), "--");
          else std::snprintf(out, sizeof(out), "OUT %.2f", std::max(0.0f, -d.current[s]));
          ui::set_text(secondary_[n], out);
        } else if (n == NODE_BATTERY) {
          char discharge[32];
          if (!d.current_ok[s]) std::snprintf(discharge, sizeof(discharge), "--");
          else std::snprintf(discharge, sizeof(discharge), "DIS %.2f", std::max(0.0f, -d.current[s]));
          ui::set_text(secondary_[n], discharge);
        }
      }
      if (d.battery_soc_ok) std::snprintf(price_text, sizeof(price_text), "%.0f%%", d.battery_soc);
      else std::snprintf(price_text, sizeof(price_text), "--%%");
      ui::set_text(soc_, price_text);
      if (d.history_error) ui::set_text(status_, plugin_->text("no_history"));
      else ui::set_text(status_, plugin_->text(compact_nodes_ ? "live_hint_compact" : "live_hint"));
    }
    update_flows(epoch);
  }

  void update_flows(uint32_t epoch) {
    const FlowData &d = plugin_->data();
    if (compact_layout_) return;
    const float solar = d.current_ok[0] ? std::max(0.0f, d.current[0]) : 0;
    const float battery = d.current_ok[1] ? d.current[1] : 0;  // positive charges, negative discharges
    const float grid = d.current_ok[2] ? d.current[2] : 0;      // positive imports, negative exports
    const float home = d.current_ok[3] ? std::max(0.0f, d.current[3]) : 0;
    const float car = d.current_ok[4] ? std::max(0.0f, d.current[4]) : 0;
    const float heat_pump = d.current_ok[5] ? std::max(0.0f, d.current[5]) : 0;
    const float charge = std::max(0.0f, battery);
    const float discharge = std::max(0.0f, -battery);
    // Estimate source routing from the selected totals: solar surplus charges the battery first;
    // any remaining charge is attributed to grid import.
    const float solar_to_battery = std::min(charge, std::max(0.0f, solar - home));
    const float grid_to_battery = std::max(0.0f, charge - solar_to_battery);
    const float flow[FLOWS] = {
        std::max(0.0f, solar - solar_to_battery), solar_to_battery,
        std::max(0.0f, std::max(0.0f, grid) - grid_to_battery), grid_to_battery,
        discharge, car, std::max(0.0f, -grid), heat_pump};
    for (int i = 0; i < FLOWS; i++) {
      const int x0 = connector_points_[i][0].x, y0 = connector_points_[i][0].y;
      const int x1 = connector_points_[i][1].x, y1 = connector_points_[i][1].y;
      const float phase = std::fmod((epoch % 4) / 4.0f + i * 0.13f, 1.0f);
      const float p = phase;
      lv_obj_set_pos(dots_[i], static_cast<int>(x0 + (x1 - x0) * p) - ui::px(4),
                     static_cast<int>(y0 + (y1 - y0) * p) - ui::px(4));
      if (!daily_ && flow[i] > 0.01f) lv_obj_clear_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
      else lv_obj_add_flag(dots_[i], LV_OBJ_FLAG_HIDDEN);
    }
  }

  SigenFlow *plugin_;
  lv_obj_t *title_{}, *price_{}, *mode_pill_{}, *mode_text_{}, *status_{}, *compact_status_{};
  lv_obj_t *circles_[SERIES]{}, *icon_[SERIES]{}, *names_[SERIES]{}, *values_[SERIES]{}, *secondary_[SERIES]{}, *soc_{};
  lv_obj_t *compact_cards_[SERIES]{}, *compact_values_[SERIES]{};
  lv_obj_t *connectors_[FLOWS]{}, *dots_[FLOWS]{};
  lv_point_precise_t connector_points_[FLOWS][2]{};
  tessera::Point centers_[SERIES]{};
  int width_ = 0, height_ = 0, home_d_ = 0, mode_h_ = 0;
  uint32_t day_seen_ = 0;
  bool daily_ = false, compact_nodes_ = false, compact_layout_ = false;
};

void SigenFlow::setup() {
  flow_ = new FlowData();
  add_tile("today", [this]() { return new TodayTile(this); });
}

}  // namespace esphome::tessa_my_sigen_flow
