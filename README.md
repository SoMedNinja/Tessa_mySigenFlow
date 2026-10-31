# Tessa mySigen Energy

A native Tessera energy-flow tile inspired by Home Assistant's Energy view and the linked Tessera Energy-card design. The dark diagram uses coloured circles, connecting lines and moving dots for:

- Solar generation
- Grid import or export
- Battery charge or discharge
- House consumption
- Car charging

Tap anywhere on the tile to switch between **Live** power in kW and **Today** totals in kWh. In daily mode, grid import/export and battery charge/discharge are shown separately.

## Setup

1. Add this repository in Tessera's **Plugins** page and install it on a screen with PSRAM.
2. In the plugin's **On this screen** settings, select the five Home Assistant power sensors (Solar, Battery, Grid, Home and Car), then save and build the screen.
3. Add **Energy flow** to the layout. Choose **3 × 2** or **3 × 3** cells.

The plugin uses screen-level entity inputs to work with Tessera versions whose manifest validator does not support per-tile entity options. The selected entities are read in watts and displayed in kW. The car sensor should report positive power while charging. For grid power, positive means import and negative means export. For battery power, positive means discharge and negative means charge.

## Today's totals

Daily energy is integrated from Home Assistant's five-minute mean power statistics: mean kW × interval hours. The current five-minute slot is weighted by the elapsed portion of the slot. Home Assistant restores recorder history after a screen restart, so totals do not depend on a buffer stored only on the ESP.

The selected sensors need Home Assistant recorder statistics (normally long-term statistics for power sensors with `state_class: measurement` and unit W). If statistics are missing, the tile reports that history is unavailable and keeps displaying live power when the sensors are available.

## Device and memory

The plugin is native C++ built into the ESPHome firmware. It draws with Tessera's LVGL line and shape primitives, uses no chart widget, browser code, web assets, or HACS frontend component on the ESP. It requires PSRAM and is intended for the Waveshare ESP32-S3-Touch-LCD-7B and other Tessera screens with PSRAM.

## Permissions and privacy

The screen reads the five entities selected in this plugin's settings and asks Home Assistant for `recorder/statistics_during_period` for those same entities. It requests today's local-day window in six-hour chunks. No data is sent to an external server.
