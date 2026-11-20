# H743 LoRa Ground Station

An ESP32-S3 ground station for ArduPilot telemetry. It receives encrypted data
from a Raspberry Pi over LoRa and displays it in a local browser dashboard.
The ESP32 creates its own Wi-Fi network, so no router or internet connection is
needed during operation.

```text
ArduPilot flight controller
        | MAVLink / UART
Raspberry Pi + Ra-01
        | Encrypted LoRa, with acknowledgements and read requests
ESP32-S3 Zero + Ra-01
        | Local Wi-Fi
Browser dashboard
```

This repository contains the **ESP32 firmware**. The aircraft runs the companion
VFPR Python project. The return link supports ping and diagnostic requests;
flight-control commands are not implemented.

## Dashboard

| View | What it shows |
| --- | --- |
| **Flight** | 2D artificial horizon, compass, attitude, GPS, altitude, speed, battery and RC input |
| **Sensors** | IMU selection, acceleration and gyroscope XYZ plots, pressure, vibration, range and optical flow |
| **Power** | Voltage, current and calculated power (`P = U * I`), with history graphs |
| **Motors** | Output commands, RC channels and available ESC feedback |
| **Health** | Sensor and EKF status, power rails, MCU temperature, memory and navigation |
| **Pipeline** | Radio state, packet counters, signal quality, requests and device diagnostics |

Live data refreshes every 250 ms; full diagnostics refresh every three seconds
while needed. New readings still depend on the LoRa update rate. Plots show
received samples, keep up to 180 samples within five minutes and reset on page
reload. Unavailable readings show `--`; cached readings retain their age.

## Hardware and wiring

- ESP32-S3 Zero with native USB and an onboard WS2812 RGB LED.
- Ai-Thinker Ra-01 / SX1278 radio with a suitable antenna.
- Matching Ra-01 on the Raspberry Pi running the VFPR sender.
- ESP-IDF **5.5.1** for building the firmware.

Use the **GPIO numbers** below, not header positions. The Ra-01 requires
**3.3 V power and logic**. Connect a common ground, keep SPI wires short and fit
the antenna before transmitting.

| Ra-01 pin | ESP32-S3 connection |
| --- | --- |
| 3.3V | Regulated 3.3 V |
| GND | GND |
| MOSI | GPIO11 |
| MISO | GPIO13 |
| SCK | GPIO12 |
| NSS / CS | GPIO10 |
| RESET | GPIO9 |
| ANT | Antenna matched to the operating frequency |
| DIO0-DIO5 | Unconnected; firmware polls the radio |

The onboard RGB LED uses **GPIO21**. GPIO19/20 are reserved for native USB.

## Setup

### 1. Configure the shared key

The private key header is deliberately excluded from Git. After cloning,
create `main/link_key.h`:

```c
#pragma once
#define STATION_SHARED_KEY_HEX "PASTE_YOUR_64_HEX_CHARACTERS_HERE"
```

Replace the placeholder with the **same 64 hexadecimal characters** used by
`SHARED_KEY_HEX` in the Pi's private `link_key.py`. This represents a 32-byte
AES-256 key. Missing, malformed and all-zero keys are rejected.

Keep the real key and compiled firmware out of GitHub. Changing the key requires
updating both peers and rebuilding/flashing the ESP32.

### 2. Check the radio settings

Run commands from the repository root in an ESP-IDF terminal:

```text
idf.py set-target esp32s3
idf.py menuconfig
```

Open **H743 LoRa ground station** to configure pins, radio, Wi-Fi, RGB and
terminal output. Both radios must use the same frequency and modulation settings.

| Setting | Default |
| --- | --- |
| Frequency | 433.775 MHz |
| Bandwidth | 125 kHz |
| Spreading factor | SF7 |
| Coding rate | 4/5 |
| Sync word | `0x12` |
| Preamble | 8 symbols |
| TX power | 5 dBm |
| TX duty-cycle ceiling | 10% |
| Explicit header / payload CRC | Enabled |

These are configuration defaults; select settings permitted for your location.
`sdkconfig.defaults` seeds new builds. For an existing build, change the active
settings through menuconfig.

### 3. Build and flash

```text
idf.py build
idf.py -p COM_PORT flash
```

Replace `COM_PORT` with the ESP32's port, such as `COM35` on Windows or
`/dev/ttyACM0` on Linux. The first build downloads the pinned dependencies.
The project targets 4 MB flash and does not require PSRAM.

### 4. Open the dashboard

Start the VFPR sender on the Pi, then connect your phone or computer to:

| Setting | Default |
| --- | --- |
| Wi-Fi network | **VFPR-Station** |
| Wi-Fi password | `vfpr-ground` |
| Dashboard | <http://espground.local> |
| IP fallback | <http://192.168.4.1> |

Keep the Wi-Fi connection selected if your device reports no internet access.
If the friendly hostname does not resolve, use the IP address.

## RGB and terminal output

| LED indication | Meaning |
| --- | --- |
| Blue pulse | Waiting for an authenticated packet |
| Green | Uplink connected |
| Amber | Uplink degraded |
| Slow red blink | Uplink lost |
| Slow amber blink | Invalid key configuration |
| Fast red blink | Radio or station initialization error |

The webpage can toggle terminal output at runtime. To start with output off,
disable **Enable terminal debug prints at startup** in menuconfig and rebuild.

The switch controls application logs and ordinary USB status/frame/reply JSON.
**Enable it for the optional PC USB dashboard.** LoRa and the webpage continue
working with it off. ROM and bootloader messages can still appear at startup.

## Troubleshooting

Use the **Pipeline** view to inspect readiness flags, counters and link state.

| Problem | Check |
| --- | --- |
| Radio unavailable / `radio_ready: false` | 3.3 V, common ground, SPI, NSS and RESET. The SX1278 version register should read `0x12`. |
| Authentication errors | Same private key, protocol version and radio settings on both peers. |
| Link lost / no ACK | Pi sender running, antennas fitted, matching settings and signal quality. |
| FC connected but values missing | FC telemetry streams and the Pi-to-FC transmit wire. Heartbeats alone do not provide sensor data. |
| Some sensor fields stay `--` | That telemetry may require hardware or firmware support absent from the aircraft. |
| USB dashboard receives nothing | Enable terminal output, select the native USB port and close other serial readers. |
| RGB does not light | GPIO21, LED enabled, brightness above zero and `led_ready` in device diagnostics. |
| Dashboard hostname does not work | Stay connected to station Wi-Fi and open `http://192.168.4.1`. |

## Development

The link uses protocol v2 with **AES-256-GCM**, authenticated headers and LoRa
payload CRC. Session/sequence checks reject duplicates and old state within a
running session. Persistent replay protection is required before adding future
action commands.

| HTTP endpoint | Purpose |
| --- | --- |
| `GET /api/live` | Compact live telemetry and link status |
| `GET /api/telemetry` | Full telemetry and station diagnostics |
| `GET /api/status` | Alias of the full snapshot |
| `POST /api/terminal` | Same-origin JSON print toggle: `{"enabled":true}` or `{"enabled":false}` |

See [tests/README.md](tests/README.md) for hardware-free regression checks and
their prerequisites.
