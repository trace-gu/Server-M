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
