# Tessa mySigen Energy

A native Tessera energy-flow tile inspired by Home Assistant's Energy view and the linked Tessera Energy-card design. The dark diagram uses coloured circles, connecting lines and moving dots for:

- Solar generation
- Grid import and export
- Battery state of charge, charge and discharge
- House consumption
- Car charging

Tap anywhere on the tile to switch between **Live** power in kW and **Today** totals in kWh. In daily mode, grid import/export and battery charge/discharge are shown separately.

The upper-left price badge shows the current value from a selected electricity-price sensor in SEK/kWh. It updates with that sensor and does not show a time interval.

## Setup

1. Add this repository in Tessera's **Plugins** page and install it on a screen with PSRAM.
2. In the plugin's **On this screen** settings, select the current price sensor, five power sensors (Solar, Battery, Grid, Home and Car), and a battery state-of-charge sensor in percent. The price sensor should report a numeric value in SEK/kWh, for example Nord Pool's current price sensor.
3. Add **Energy flow** to the layout. Resize it from **1 × 1** through **3 × 3** cells. Small tiles switch to compact, colored metric cards; larger tiles show the full flow diagram.

The plugin uses screen-level entity inputs to work with Tessera versions whose manifest validator does not support per-tile entity options. It reads each Home Assistant power sensor's `unit_of_measurement` attribute and converts W, kW or MW to kW. For grid power, positive means import and negative means export. For the Sigen inverter battery-power sensor, positive means charge and negative means discharge. For the home load, use the Sigen plant **Consumed power** sensor; plant active power is not the home-consumption sensor. The battery state-of-charge input is optional at build time, but select it in the screen Plugins settings to show its percentage.

The diagram includes paths from solar and grid to the battery. The selected sensors report totals rather than direct source routing, so the solar/grid split for battery charging is estimated from solar surplus, house load and battery charge power.

## Today's totals

Daily energy is integrated from Home Assistant's five-minute mean power statistics: mean kW × interval hours. The current five-minute slot is weighted by the elapsed portion of the slot. Home Assistant restores recorder history after a screen restart, so totals do not depend on a buffer stored only on the ESP.

The selected power sensors need Home Assistant recorder statistics (normally long-term statistics for power sensors with `state_class: measurement` and unit W or kW). If statistics are missing, the tile reports that history is unavailable and keeps displaying live power when the sensors are available.

## Device and memory

The plugin is native C++ built into the ESPHome firmware. It draws with Tessera's LVGL line and shape primitives, uses no chart widget, browser code, web assets, or HACS frontend component on the ESP. It requires PSRAM and is intended for the Waveshare ESP32-S3-Touch-LCD-7B and other Tessera screens with PSRAM.

## Permissions and privacy

The screen reads the price, five power and battery charge entities selected in this plugin's settings. It asks Home Assistant for `recorder/statistics_during_period` for only the five power entities, in today's local-day window and six-hour chunks. No data is sent to an external server.
