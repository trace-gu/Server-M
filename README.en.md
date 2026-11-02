**English** · [简体中文](README.md)

<h1 align="center">Server-M Monitor</h1>

<p align="center">
  <strong>Turn a FoloToy AI Passport into a desktop server status display</strong><br>
  ESP32-C3 · 240 × 320 · three-button navigation · web provisioning
</p>

<p align="center">
  <a href="https://github.com/trace-gu/Server-M/releases/latest"><img src="https://img.shields.io/github/v/release/trace-gu/Server-M?style=flat-square" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-64748b?style=flat-square" alt="MIT License"></a>
</p>

Server-M is server-monitoring firmware for the FoloToy AI Passport hardware. It polls servers through a relay endpoint and shows CPU, memory, disk, network, temperature, and container metrics on the 240 × 320 display, with 24-hour trends, screen-off power saving, and switching between saved networks.

## Features

- Store up to 20 servers; the list shows online status plus CPU and memory summaries.
- The detail page shows CPU, memory, swap, disk, load, network throughput, disk I/O, uptime, temperature, TLS certificate days remaining, Docker container state, and backup status.
- Keep a 24-hour trend for each server on the device.
- Set the refresh interval from 1 to 300 seconds, brightness from 10 to 100%, and screen-off to always on.
- Store up to 6 Wi-Fi networks; reconnect automatically and rotate to the next saved network after repeated failures.
- Provision from a phone web page with no companion app and no Bluetooth.

## Hardware requirements

| Item | Requirement |
| --- | --- |
| Device | FoloToy AI Passport (ESP32-C3, 8 MB Flash, no PSRAM) |
| Display | ST7789P3, 240 × 320 |
| Buttons | UP / DOWN / OK |
| Network | 2.4 GHz Wi-Fi and a host running the relay service that the device can reach |

## Server setup (1Panel / Baota)

Install the relay script once on every monitored server. The steps below use
only the panel web UI and need no SSH: the script calls the panel API, and the
device only requests `/status` with a token.

**Prerequisites**: a domain that resolves to this server, with a Let's Encrypt
certificate requested in the panel. The device accepts only HTTPS certificates
issued by a public CA.

| Step | 1Panel | Baota |
| --- | --- | --- |
| 1. Upload the script | Hosts → Files → `/opt` → create `ai-passport` → upload `server/ai_passport_agent.py` from this repository | Files → `/opt` → create `ai-passport` → upload the same file |
| 2. Enable the API | Panel settings → API interface → enable → copy the key | Panel settings → API interface → enable → copy the key, allow `127.0.0.1` in the IP allowlist |
| 3. Install with a scheduled task | Scheduled tasks → Create → type "Shell script" → schedule "daily" → paste the command below → save, then run it once | Scheduled tasks → Add → type "Shell script" → schedule "daily" → paste the command → save, then run it once |
| 4. Create a site | Websites → Create → type "static" → domain `monitor.example.com` | Websites → Add site → PHP "static" → domain `monitor.example.com` |
| 5. Reverse proxy | Site settings → Reverse proxy → add target `http://127.0.0.1:8899` | Settings → Reverse proxy → add target URL `http://127.0.0.1:8899`, send domain `$host`, disable caching |
| 6. Certificate | Websites → HTTPS → Let's Encrypt → enable "force HTTPS" | SSL → Let's Encrypt → enable "force HTTPS" |
| 7. Verify | Create a temporary scheduled task that runs the `curl` command below and read its log | Same |

Commands for step 3 (replace the key copied in step 2):

```bash
# 1Panel
python3 /opt/ai-passport/ai_passport_agent.py install \
    --panel 1panel --api-key '<API key>'

# Baota
python3 /opt/ai-passport/ai_passport_agent.py install \
    --panel baota --api-key '<API key>'
```

Open the task log after it runs and copy the value on the access-token line.
Save it and disable or delete the task. systemd keeps the agent running, so the
task does not need to run repeatedly.

Verification command for step 7:

```bash
curl -s -H "X-Api-Token: <access token>" http://127.0.0.1:8899/status
```

A JSON response containing `"online": true` means the setup works. Opening
`https://monitor.example.com/status` in a browser shows `unauthorized`; that is
expected because browsers do not send `X-Api-Token`.

Finally, enter `https://monitor.example.com/status` and the same token in the
device portal's Server card. Repeat these steps for each server, using its own
domain and token.

> [!NOTE]
> If the 1Panel reverse-proxy container cannot reach the host's `127.0.0.1`,
> point the proxy target at the server's LAN IP (for example
> `http://192.168.1.10:8899`), reinstall with `--bind 0.0.0.0`, and use a
> firewall to allow only the proxy host to reach that port.

See the [relay server guide](server/README.md) for endpoint fields, manual
installation, and troubleshooting.

## Flash the prebuilt firmware

1. Download `Server-M-full.bin` from [Releases](https://github.com/trace-gu/Server-M/releases/latest).
2. Connect the device to a computer with a data-capable USB cable and install `esptool`:

```bash
python -m pip install esptool
```

3. Write the merged image starting at `0x0`:

```bash
python -m esptool --chip esp32c3 -p <port> -b 460800 write_flash 0x0 Server-M-full.bin
```

`<port>` is a name such as `COM3` on Windows, or `/dev/tty.usbmodem*` or `/dev/ttyACM*` on macOS/Linux.

> [!WARNING]
> Writing a merged image at `0x0` overwrites the application and NVS data, clearing saved servers, Wi-Fi networks, and display settings. This firmware does not enable secure boot or flash encryption, so no original-firmware backup is required.

## First-time provisioning

1. On first boot with no saved network, the device enters provisioning mode and shows the access point `AI-Passport-XXXX` plus a random 8-character password generated for this boot.
2. Connect the phone to that access point and open `http://192.168.4.1`.
3. Fill in the cards in order: **Server → Display settings → Wi-Fi**.
4. Select "Connect and exit provisioning". The device saves the configuration, closes the access point, and joins the Wi-Fi network directly.
5. After provisioning, find the new IP on the device under Settings → Wi-Fi networks, or look for the hostname `ai-passport` in the router's client list.

See the [web provisioning guide](docs/development/engineering/wifi-provisioning.md) for the flow, HTTP endpoints, and acceptance checks.

## Build from source

ESP-IDF 5.5.3 and Python 3 are required.

```bash
git clone https://github.com/trace-gu/Server-M.git
cd Server-M
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build
```

Generate and verify a flashable merged image:

```bash
./tools/validate.sh --firmware
```

The artifact is `build/Server-M-full.bin`, flashed at offset `0x0`. Run only the static checks and host tests with:

```bash
./tools/validate.sh --static
```

See [environment setup](docs/development/engineering/environment-setup.md) for prerequisites and [build and test](docs/development/engineering/build-and-test.md) for the complete gate.

## Buttons

| Screen | UP | DOWN | OK | Long-press OK |
| --- | --- | --- | --- | --- |
| Server list | Previous server | Next server | Open detail | Open settings |
| Server detail | Previous server | Next server | Refresh now | Back to list |
| Settings | Move up | Move down | Change the selected item | Back to list |
| Wi-Fi networks | Move up | Move down | Switch network or start provisioning | Back to settings |

When the screen is off, any button wakes it and reconnects Wi-Fi before rendering.

## Repository layout

```text
components/bsp/  Board support: display, buttons, audio, battery, shared I2C
main/            Server-M application: UI, configuration, Wi-Fi state, HTTP client, and portal
tests/           Host-runnable pure-logic tests
tools/           Validation, firmware merge, and archive scripts
docs/            Development, hardware, contribution guides, and community references
```

## Documentation

| Document | Content |
| --- | --- |
| [Project overview](docs/README.md) | Repository scope, capabilities, and documentation index |
| [Web provisioning](docs/development/engineering/wifi-provisioning.md) | Portal flow, endpoints, and acceptance |
| [Hardware development guide](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) | Pins, buses, display, and audio constraints |
| [Build and test](docs/development/engineering/build-and-test.md) | Gates, host tests, and firmware verification |
| [AI development entry](AGENTS.md) | Mandatory task routing for AI agents |
| [Changelog](docs/CHANGELOG.md) | Version history |

## License

[MIT](LICENSE)
