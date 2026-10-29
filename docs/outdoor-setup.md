# Outdoor Sensor Setup (Phoenix / Desert Climate)

Guidance for placing ESP32 + DHT11 nodes outside in hot, dry (and occasionally monsoon) conditions.

## Current outdoor node role

- Same firmware as indoor nodes (`outdoor1` / `outdoor2` PlatformIO environments).
- Reports temperature and humidity to the hub every ~30 s.
- Hub uses outdoor data for the open-windows recommendation (must be cooler outside by `TEMP_DELTA_C` and outdoor RH ≤ `MAX_OUTDOOR_RH`).

You can run with a single outdoor node; a second improves coverage (e.g. front vs back of house).

---

## Enclosure

- Prefer a small **IP65** (or better) plastic box (e.g. ~100×68×50 mm).
- Drill several small holes on the **bottom or shaded side** so air can reach the DHT11. Sealed boxes will measure box temperature, not outdoor air.
- Mount the DHT11 near the vents; keep the ESP32 higher or shielded from direct rain splash.
- **Avoid metal boxes** — they block Wi‑Fi.

## Sun and heat

- Place in **shade** when possible (north wall, under eaves, under a roof overhang).
- Direct sun heats the enclosure far above true air temperature and makes readings useless for “open windows?” decisions.
- Light-colored or white enclosures reflect more heat than dark ones.
- Phoenix summer ambient can exceed 40–45 °C; the DHT11 range is roughly 0–50 °C — shade and ventilation matter.

## Power options

1. **USB cable run (simplest)**  
   Outdoor-rated USB cable to an indoor supply, or a weatherproof outdoor GFCI outlet + USB adapter. Reliable for always-on nodes.

2. **USB power bank**  
   Fine for testing or short periods; plan on periodic recharging.

3. **Solar + battery**  
   Small 5–6 V panel + charge controller (e.g. TP4056-class or better) + cell (LiPo / 18650) + 5 V boost to the ESP32.  
   Later: add deep sleep on the sensor firmware to stretch runtime.

Keep the USB cable and connectors protected from UV and monsoon rain.

## Mounting and radio

- Zip ties, outdoor screws, or magnetic mounts on non-critical surfaces.
- Keep the onboard antenna away from large metal objects (gutters, AC units, foil-faced insulation).
- Check **RSSI** on the sensor Serial Monitor (printed with each reading). Rough guide:
  - Better than −70 dBm → good
  - −70 to −80 dBm → usable but may see occasional POST retries
  - Worse than −80 dBm → move closer to the router or hub, or adjust placement

The sensor firmware retries failed POSTs a few times with backoff.

## Wiring reminder (outdoor node)

Same as indoor:

| DHT11 | ESP32  |
| ----- | ------ |
| VCC   | 3.3V   |
| GND   | GND    |
| DATA  | GPIO 4 |

Power the module from 3.3V unless your breakout is explicitly 5V-tolerant with its own regulator.

## Monsoon / rain

- Orient vent holes downward so water does not pool on the sensor.
- A small drip loop on any cable entering the box helps keep water out.
- After heavy rain, verify the node still reports and that humidity/temperature look plausible (not stuck at 99% or board-heat values).

## Future sensor upgrade

When moving to **BME280 / BME680 / SHT4x** (pressure + better accuracy):

- Same enclosure and power approach still apply.
- Update the sensor-node firmware (library + JSON fields).
- Hub recommendation logic can later incorporate pressure or dew-point if desired.

See the main [README](../README.md) for flashing environments (`outdoor1`, `outdoor2`), dashboard use, and troubleshooting.
