# Changelog

## 0.1.6

- Draw the timeline with LVGL line objects because the screen firmware disables the chart widget.
- Fix the missing numeric parser and day rollover field reported by the firmware compiler.

## 0.1.5

- Use LVGL chart range API available in the Tessera firmware toolchain.

## 0.1.4

- Replace the removed LVGL chart refresh call with object invalidation for current LVGL.

## 0.1.3

- Use Tessera list-wildcard syntax to map the recorder statistics object and samples.

## 0.1.2

- Fix recorder statistics answer paths for Tessera field-path syntax.

## 0.1.1

- Use screen-level sensor inputs for compatibility with Tessera validators that predate per-tile entity options.
- Read selected sensor values through ESPHome Home Assistant sensors and keep the fixed 3 × 2 chart.

## 0.1.0

- Initial release: fixed 3 × 2 power metrics tile with five sensor selectors.
- Load today's five-minute mean power history from Home Assistant recorder statistics.
- Add touch selection for the time and all five sensor readings.
