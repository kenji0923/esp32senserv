# esp32senserv

ESP32 sensor server plus a local FastAPI web console.

This project has two parts:

- ESP32 firmware in `main/main.c`
- Web server in `web_server.py`

The ESP32 side acts as an ESP-NOW server. It stores per-client configuration in NVS and sends that configuration to sensor clients. The web server connects to the ESP32 serial console, parses `ls`, `show`, and `DATA` output, and provides a browser UI.

## Features

- Per-client configuration stored on the ESP32 server
- Auto-sync of client settings from serial console output
- Web UI for editing client settings
- SQLite persistence in `configs.db` for:
  - client `name`
  - client `log_enabled`
  - client `tlg_bucket`
  - global Telegraf host / port / protocol
- Optional Telegraf forwarding from the web server

## Current Config Fields

The ESP32 server sends these fields to each client:

- `sleep_sec`
- `work_delay_ms`
- `batt_avg`
- `sht_avg`
- `sht_read_wait_time_ms`
- `dps_osr`
- `dps_avg`
- `dps_read_wait_time_ms`
- `config_hash`

Current validation rules in the server firmware:

- `sleep_sec`: `0..86400`
- `work_delay_ms`: `1..65535`
- `batt_avg`: `1..255`
- `sht_avg`: `1..255`
- `sht_read_wait_time_ms`: `1..65535`
- `dps_osr`: `0..7`
- `dps_avg`: `1..255`
- `dps_read_wait_time_ms`: `1..65535`

## Web UI Notes

The web UI is served from `static/index.html`.

- `Full Sync with ESP32` sends `ls` and `show` commands to refresh the displayed config.
- `WRITE MODIFIED TO SERVER` sends changed config values to the ESP32 server console.
- Client log checkbox and bucket are persisted immediately in SQLite.
- Telegraf settings are persisted immediately in SQLite when changed.

## Telegraf Fields

When logging is enabled for a client, the web server forwards these measurement fields:

- `v_batt`
- `temperature`
- `humidity`
- `pressure`
- `temphumid_validcount`
- `temphumid_trial`
- `pressure_validcount`
- `pressure_trial`

## Build And Flash Server Firmware

Typical ESP-IDF flow:

```bash
idf.py build
idf.py flash
```

Run those in this directory:

```bash
cd /home/kshu/work/development/esp32senserv
```

## Run Web Server

From this directory:

```bash
python3 web_server.py
```

The web UI is served on port `8082` by default.

Serial settings are currently configured in `web_server.py`:

- port: `/dev/ttyUSB2`
- baud: `115200`

## Example systemd Service

Example unit file:

```ini
[Unit]
Description=ESP32 Sensor Web Console
After=network.target

[Service]
Type=simple
User=YOUR_USER
WorkingDirectory=/path/to/esp32senserv
ExecStart=/usr/bin/python3 /path/to/esp32senserv/web_server.py
Restart=always
RestartSec=2

[Install]
WantedBy=multi-user.target
```

Typical setup:

1. Save the file as `/etc/systemd/system/esp32senserv-web.service`
2. Replace `YOUR_USER` and `/path/to/esp32senserv` with real values
3. Reload systemd:

```bash
sudo systemctl daemon-reload
```

4. Enable and start the service:

```bash
sudo systemctl enable --now esp32senserv-web.service
```

5. Check logs:

```bash
journalctl -u esp32senserv-web.service -f
```

## Storage

- ESP32 server NVS keys:
  - `db_schema`
  - `db`
  - `count`
- Web server SQLite database:
  - `configs.db`

## Important Compatibility Note

The server firmware, client firmware, and web server parser all depend on the same wire-format for `config_data_t` and `sensor_data_t`.

If you change those structs, update:

- `esp32senserv/main/main.c`
- `esp32sensclient/main/main.c`
- `esp32senserv/web_server.py`

and reflash the affected ESP32 firmware.
