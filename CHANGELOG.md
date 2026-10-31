# Changelog

## 0.2.7

- Match the bubbles and price text to Tessera's light and dark themes for readable contrast.
- Make the top-right Live/Today switch a larger, directly tappable button.
- Keep bubble interiors on the screen's surface and preserve legible icon and reading contrast.

## 0.2.6

- Stop flow lines at the bubble edges and use solid, clean bubble interiors.
- Show a plain current-price line in Live mode and today's recorder-based average price in Today mode.
- Make the Live/Today tap action clearer and refine the header and colors.

## 0.2.5

- Correct the Sigen battery sensor direction: positive is charging and negative is discharging.
- Allow an initial build before a battery state-of-charge entity is selected.

## 0.2.4

- Separate grid import/export and battery charge/discharge readings; show battery state of charge.
- Detect W, kW or MW sensor units and add estimated solar/grid charging paths to the battery.
- Refine node spacing and colors to keep icons and values apart.

## 0.2.3

- Allow resizing from 1 × 1 through 3 × 3 cells, with a compact layout for small tiles.

## 0.2.2

- Resize the tile from 2 × 2 to 3 × 3 cells and condense the readings in smaller layouts.

## 0.2.1

- Add a selected current price sensor and display its live SEK/kWh value in a highlighted header badge.

## 0.2.0

- Replace the price-like timeline with an interactive energy flow diagram inspired by Tessera's Energy card.
- Add live and today's total modes for solar, grid, battery, home and car power.
- Limit layouts to 3 × 2 or 3 × 3 tiles.
- Add a highlighted upper-left current electricity price badge in SEK/kWh.

## 0.1.7

- Allow tile sizes from 2 × 1 through 3 × 3 and use a compact layout for short tiles.

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
