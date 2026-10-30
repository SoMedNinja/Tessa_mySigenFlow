# Changelog

## 0.1.1

- Use screen-level sensor inputs for compatibility with Tessera validators that predate per-tile entity options.
- Read selected sensor values through ESPHome Home Assistant sensors and keep the fixed 3 × 2 chart.

## 0.1.0

- Initial release: fixed 3 × 2 power metrics tile with five sensor selectors.
- Load today's five-minute mean power history from Home Assistant recorder statistics.
- Add touch selection for the time and all five sensor readings.
