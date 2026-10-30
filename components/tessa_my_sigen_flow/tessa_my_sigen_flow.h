#pragma once
#include "esphome/core/component.h"
#include "esphome/components/smart_display/plugin_api.h"

namespace esphome::tessa_my_sigen_flow {

struct FlowData;

class SigenFlow : public Component, public tessera::Plugin {
 public:
  void setup() override;
  float get_setup_priority() const override { return setup_priority::DATA; }
  void on_message(JsonObjectConst message) override;
  void configure(uint32_t epoch);
  void update_sensor(uint8_t series, float value, const char *const entities[5], uint32_t epoch);
  bool sample(uint32_t epoch);
  const FlowData &data() const;

 private:
  void request_next();
  FlowData *flow_ = nullptr;
  uint32_t request_id_ = 0;
  uint32_t request_start_ = 0;
  uint32_t request_end_ = 0;
  uint32_t history_until_ = 0;
  uint8_t request_series_ = 0;
  uint8_t job_ = 0;
  uint8_t jobs_ = 0;
};

}  // namespace esphome::tessa_my_sigen_flow
