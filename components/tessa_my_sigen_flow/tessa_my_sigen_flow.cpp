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
constexpr const char *LABELS[SERIES] = {"solar", "battery", "grid", "home", "car"};
constexpr uint32_t COLORS[SERIES] = {0xF2C94C, 0x43D9D0, 0x6EA8FE, 0xB28DFF, 0xFF9F68};

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

static std::string value_text(float value, bool ok) {
  if (!ok || !std::isfinite(value)) return "--";
  char out[20];
  std::snprintf(out, sizeof(out), "%.2f kW", value);
  return out;
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

class TodayTile : public tessera::Tile {
 public:
  explicit TodayTile(SigenFlow *plugin) : plugin_(plugin) {}

  void create(const tessera::TileContext &c) override {
    width_ = c.width;
    height_ = c.height;
    compact_ = height_ < ui::px(210);
    plugin_->configure(tessera::epoch());

    title_ = ui::label(c.parent, Font::HEADLINE, theme::INK);
    time_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_set_style_text_align(time_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_remove_flag(title_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(time_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(title_, width_ * 2 / 3, ui::line_height(Font::HEADLINE));
    lv_obj_set_pos(title_, 0, 0);
    lv_obj_set_size(time_, width_ / 3, ui::line_height(Font::BODY));
    lv_obj_set_pos(time_, width_ * 2 / 3, ui::px(3));

    const Font value_font = compact_ ? Font::BODY : Font::TITLE;
    const int top = ui::line_height(Font::HEADLINE) + (compact_ ? ui::px(4) : ui::px(8));
    const int columns = width_ / SERIES;
    for (int s = 0; s < SERIES; s++) {
      names_[s] = ui::label(c.parent, Font::BODY, theme::SUBTLE);
      values_[s] = ui::label(c.parent, value_font, theme::INK);
      lv_obj_remove_flag(names_[s], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_remove_flag(values_[s], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_text_color(names_[s], lv_color_hex(COLORS[s]), 0);
      lv_obj_set_size(names_[s], columns - ui::px(4), ui::line_height(Font::BODY));
      lv_obj_set_pos(names_[s], s * columns, top);
      lv_obj_set_size(values_[s], columns - ui::px(4), ui::line_height(value_font));
      lv_obj_set_pos(values_[s], s * columns, top + ui::line_height(Font::BODY));
    }

    chart_x_ = ui::px(2);
    chart_y_ = top + ui::line_height(Font::BODY) + ui::line_height(value_font) + (compact_ ? ui::px(4) : ui::px(8));
    chart_w_ = width_ - ui::px(4);
    chart_h_ = compact_
        ? std::max(ui::px(24), height_ - chart_y_ - ui::line_height(Font::BODY) - ui::px(6))
        : std::max(ui::px(70), height_ - chart_y_ - 2 * ui::line_height(Font::BODY) - ui::px(10));
    for (int i = 0; i < 4; i++) {
      grid_[i] = lv_line_create(c.parent);
      lv_obj_remove_flag(grid_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_line_width(grid_[i], std::max(1, ui::px(1)), 0);
      lv_obj_set_style_line_color(grid_[i], lv_color_hex(0x344253), 0);
      grid_points_[i][0] = {chart_x_, chart_y_ + chart_h_ * i / 3};
      grid_points_[i][1] = {chart_x_ + chart_w_, chart_y_ + chart_h_ * i / 3};
      lv_line_set_points(grid_[i], grid_points_[i], 2);
      vertical_grid_[i] = lv_line_create(c.parent);
      lv_obj_remove_flag(vertical_grid_[i], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_set_style_line_width(vertical_grid_[i], std::max(1, ui::px(1)), 0);
      lv_obj_set_style_line_color(vertical_grid_[i], lv_color_hex(0x344253), 0);
      const int x = chart_x_ + chart_w_ * i / 3;
      vertical_points_[i][0] = {x, chart_y_};
      vertical_points_[i][1] = {x, chart_y_ + chart_h_};
      lv_line_set_points(vertical_grid_[i], vertical_points_[i], 2);
    }
    for (int s = 0; s < SERIES; s++) {
      series_line_[s] = lv_line_create(c.parent);
      lv_obj_remove_flag(series_line_[s], LV_OBJ_FLAG_CLICKABLE);
      lv_obj_add_flag(series_line_[s], LV_OBJ_FLAG_HIDDEN);
      lv_obj_set_style_line_width(series_line_[s], std::max(2, ui::px(2)), 0);
      lv_obj_set_style_line_rounded(series_line_[s], true, 0);
      lv_obj_set_style_line_color(series_line_[s], lv_color_hex(COLORS[s]), 0);
    }

    const int axis_y = chart_y_ + chart_h_ + ui::px(2);
    for (int i = 0; i < 4; i++) {
      axis_[i] = ui::label(c.parent, Font::BODY, theme::SUBTLE);
      lv_obj_remove_flag(axis_[i], LV_OBJ_FLAG_CLICKABLE);
      const char *labels[] = {"00", "06", "12", "18"};
      ui::set_text(axis_[i], labels[i]);
      const int x = chart_x_ + chart_w_ * i / 4;
      lv_obj_set_size(axis_[i], ui::text_width(labels[i], Font::BODY) + 2, ui::line_height(Font::BODY));
      lv_obj_set_pos(axis_[i], x, axis_y);
    }
    status_ = ui::label(c.parent, Font::BODY, theme::SUBTLE);
    lv_obj_remove_flag(status_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_size(status_, width_ - ui::px(4), ui::line_height(Font::BODY));
    lv_obj_set_pos(status_, 0, axis_y + ui::line_height(Font::BODY));
    if (compact_) lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
    selected_line_ = lv_line_create(c.parent);
    lv_obj_remove_flag(selected_line_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_line_width(selected_line_, std::max(1, ui::px(1)), 0);
    lv_obj_set_style_line_color(selected_line_, lv_color_hex(0xFFFFFF), 0);
    lv_obj_add_flag(selected_line_, LV_OBJ_FLAG_HIDDEN);
    day_seen_ = plugin_->data().day_start;
    redraw();
  }

  void on_tick(uint32_t epoch) override {
    plugin_->configure(epoch);
    if (day_seen_ != plugin_->data().day_start) {
      day_seen_ = plugin_->data().day_start;
      selected_ = -1;
      lv_obj_add_flag(selected_line_, LV_OBJ_FLAG_HIDDEN);
    }
    const bool live_changed = plugin_->sample(epoch);
    if (generation_ != plugin_->data().generation) redraw();
    else if (live_changed) update_live_slot();
    if (selected_ < 0) update_values(-1, epoch);
  }

  void on_theme() override { redraw(); }

  void on_tap() override {
    const tessera::Point p = tessera::tap_point();
    if (p.x < chart_x_ || p.x >= chart_x_ + chart_w_ || p.y < chart_y_ || p.y >= chart_y_ + chart_h_) return;
    const FlowData &d = plugin_->data();
    if (d.points <= 0 || !d.day_start) return;
    selected_ = std::clamp<int>((p.x - chart_x_) * d.points / chart_w_, 0, d.points - 1);
    update_values(selected_, d.day_start + selected_ * SAMPLE_SECONDS);
    lv_point_precise_t line[2] = {{p.x, chart_y_}, {p.x, chart_y_ + chart_h_}};
    lv_line_set_points(selected_line_, line, 2);
    lv_obj_clear_flag(selected_line_, LV_OBJ_FLAG_HIDDEN);
  }

 private:
  void update_values(int slot, uint32_t epoch) {
    const FlowData &d = plugin_->data();
    if (slot >= 0) {
      const tessera::LocalTime local = tessera::local_time(epoch);
      char text[8];
      std::snprintf(text, sizeof(text), "%02d:%02d", static_cast<int>(local.hour), static_cast<int>(local.minute));
      ui::set_text(time_, text);
      for (int s = 0; s < SERIES; s++) ui::set_text(values_[s], value_text(d.values[s][slot], std::isfinite(d.values[s][slot])));
      ui::set_text(status_, plugin_->text("selected_hint"));
      return;
    }
    ui::set_text(time_, plugin_->text("live"));
    for (int s = 0; s < SERIES; s++) ui::set_text(values_[s], value_text(d.current[s], d.current_ok[s]));
    if (d.history_error) ui::set_text(status_, plugin_->text("no_history"));
    else if (ready_points_ == 0) ui::set_text(status_, plugin_->text("loading_history"));
    else ui::set_text(status_, plugin_->text("today_hint"));
  }

  void redraw() {
    const FlowData &d = plugin_->data();
    ui::set_text(title_, plugin_->text("title"));
    for (int s = 0; s < SERIES; s++) {
      ui::set_text(names_[s], d.name[s][0] ? d.name[s] : plugin_->text(LABELS[s]));
    }
    float low = 0, high = 0;
    ready_points_ = 0;
    for (int s = 0; s < SERIES; s++) for (int i = 0; i < d.points; i++) {
      const float value = d.values[s][i];
      if (!std::isfinite(value)) continue;
      low = std::min(low, value);
      high = std::max(high, value);
      ready_points_++;
    }
    if (high - low < 0.2f) { low -= 0.1f; high += 0.1f; }
    const float span = high - low;
    for (int s = 0; s < SERIES; s++) {
      uint32_t count = 0;
      for (int i = 0; i < d.points; i++) {
        const float value = d.values[s][i];
        if (!std::isfinite(value)) continue;
        const int x = chart_x_ + (d.points > 1 ? i * chart_w_ / (d.points - 1) : chart_w_ / 2);
        const int y = chart_y_ + chart_h_ - static_cast<int>(std::lround((value - low) * chart_h_ / span));
        series_points_[s][count++] = {x, y};
      }
      if (count >= 2) {
        lv_line_set_points(series_line_[s], series_points_[s], count);
        lv_obj_clear_flag(series_line_[s], LV_OBJ_FLAG_HIDDEN);
      } else {
        lv_obj_add_flag(series_line_[s], LV_OBJ_FLAG_HIDDEN);
      }
    }
    if (selected_ >= 0 && selected_ < d.points && d.points > 0) {
      const int x = chart_x_ + (selected_ * 2 + 1) * chart_w_ / (d.points * 2);
      lv_point_precise_t line[2] = {{x, chart_y_}, {x, chart_y_ + chart_h_}};
      lv_line_set_points(selected_line_, line, 2);
      lv_obj_clear_flag(selected_line_, LV_OBJ_FLAG_HIDDEN);
    }
    generation_ = d.generation;
    if (selected_ < 0) update_values(-1, tessera::epoch());
  }

  void update_live_slot() {
    redraw();
  }

  SigenFlow *plugin_;
  lv_obj_t *title_{}, *time_{}, *names_[SERIES]{}, *values_[SERIES]{}, *axis_[4]{}, *status_{}, *selected_line_{};
  lv_obj_t *grid_[4]{}, *vertical_grid_[4]{}, *series_line_[SERIES]{};
  lv_point_precise_t grid_points_[4][2]{}, vertical_points_[4][2]{}, series_points_[SERIES][MAX_POINTS]{};
  int width_ = 0, height_ = 0, chart_x_ = 0, chart_y_ = 0, chart_w_ = 0, chart_h_ = 0;
  int selected_ = -1, ready_points_ = 0;
  uint32_t generation_ = 0, day_seen_ = 0;
  bool compact_ = false;
};

void SigenFlow::setup() {
  flow_ = new FlowData();
  add_tile("today", [this]() { return new TodayTile(this); });
}

}  // namespace esphome::tessa_my_sigen_flow
