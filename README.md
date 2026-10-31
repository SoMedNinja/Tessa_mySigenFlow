# Tessa mySigen Flow

A Tessera tile inspired by the **Power Metrics · Today** graph in Sigenergy's Home Assistant flow dashboards. It plots five power sensors together as soft, coloured lines in an adjustable **2 × 1 to 3 × 3** tile:

- Solar
- Battery
- Grid
- Home
- Car charging

Tap anywhere along the graph to select that time. The header then shows the five values for the selected five-minute interval and its local `HH:MM` start time. Tap a different point to inspect another time.

## Setup

1. Add this repository in Tessera's **Plugins** page and install it on a screen with PSRAM.
2. In the plugin's **On this screen** settings, select the five Home Assistant power sensors (Solar, Battery, Grid, Home, Car), then save and build the screen.
3. Add **Power metrics · Today** to the layout and resize it between 2 × 1 and 3 × 3 cells. The tile uses a compact reading layout at short heights; sensor choices are screen-level because older Tessera validators do not support per-tile entity options.

The configured Sigenergy power entities are read in watts and normalized to kW for the graph. Battery and grid values keep the sign supplied by the sensor, so charging/export can appear below zero when the integration uses negative values for those directions.

## Today's history

The plugin asks Home Assistant's recorder for five-minute mean statistics for the selected entities. Home Assistant supplies today's history again after an ESP restart; the screen does not need to keep a separate all-day buffer. Values shown for a selected point are the recorder's mean power for that five-minute interval, not a one-second instantaneous reading. This version expects the selected sensors to report power in W.

The selected sensors need Home Assistant long-term statistics (normally power sensors with `state_class: measurement` and a supported unit such as W or kW). If a sensor has no recorder statistics, that series remains empty and the tile reports the history error while still showing its live value. The EV sensor may come from the Sigenergy integration or a separate charger integration.

## Device and memory

The plugin is native C++ built into the ESPHome firmware. It makes no browser request, loads no web assets, and requires no HACS frontend component on the ESP. Its timeline buffer uses Tessera's PSRAM-first list and the manifest requires PSRAM. It is intended for the Waveshare ESP32-S3-Touch-LCD-7B and other Tessera screens with PSRAM.

## Permissions and privacy

The screen asks Home Assistant for `recorder/statistics_during_period` for only the five entity IDs selected in this tile. It sends the current local day's start and end times and requests five-minute means. No data is sent to an external server.
