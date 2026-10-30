# Tessa mySigen Flow

A Tessera tile inspired by the **Power Metrics · Today** graph in Sigenergy's Home Assistant flow dashboards. It plots five power sensors together as soft, coloured lines in a fixed **3 × 2** tile:

- Solar
- Battery
- Grid
- Home
- Car charging

Tap anywhere along the graph to select that time. The header then shows the five values for the selected five-minute interval and its local `HH:MM` start time. Tap a different point to inspect another time.

## Setup

1. Add this repository in Tessera's **Plugins** page and install it on a screen with PSRAM.
2. Add **Power metrics · Today** to the layout. The tile is fixed at 3 × 2 cells.
3. Select the five matching Home Assistant power sensors in the tile inspector.

Values are normalized to kW for the graph. Sensor names and current values come from the selected entities. Battery and grid values keep the sign supplied by the sensor, so charging/export can appear below zero when the integration uses negative values for those directions.

## Today's history

The plugin asks Home Assistant's recorder for five-minute mean statistics for the selected entities. Home Assistant supplies today's history again after an ESP restart; the screen does not need to keep a separate all-day buffer. Values shown for a selected point are the recorder's mean power for that five-minute interval, not a one-second instantaneous reading.

The selected sensors need Home Assistant long-term statistics (normally power sensors with `state_class: measurement` and a supported unit such as W or kW). If a sensor has no recorder statistics, that series remains empty and the tile reports the history error while still showing its live value. The EV sensor may come from the Sigenergy integration or a separate charger integration.

## Device and memory

The plugin is native C++ built into the ESPHome firmware. It makes no browser request, loads no web assets, and requires no HACS frontend component on the ESP. Its timeline buffer uses Tessera's PSRAM-first list and the manifest requires PSRAM. It is intended for the Waveshare ESP32-S3-Touch-LCD-7B and other Tessera screens with PSRAM.

## Permissions and privacy

The screen asks Home Assistant for `recorder/statistics_during_period` for only the five entity IDs selected in this tile. It sends the current local day's start and end times and requests five-minute means. No data is sent to an external server.
