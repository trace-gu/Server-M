**English** · [简体中文](/docs/README.zh_CN.md)

<h1 align="center">FoloToy AI PASSPORT</h1>

<p align="center">
  <picture>
    <source media="(prefers-color-scheme: dark)" srcset="../assets/images/logo-wordmark-dark.png">
    <img src="../assets/images/logo-wordmark.png" alt="FoloToy wordmark" width="128">
  </picture>
</p>

<p align="center">
  <strong>Wear it. Flash it. Make it anything.</strong><br>
  Simple and open. Anyone can build.
</p>

<p align="center">
  <a href="/docs/README.md"><img src="https://img.shields.io/badge/Open-firmware-14b8a6?style=flat-square" alt="Open firmware"></a>
  <a href="/docs/README.md"><img src="https://img.shields.io/badge/Wearable-AI-2563eb?style=flat-square" alt="Wearable AI"></a>
  <a href="/docs/development/ai-guide.md"><img src="https://img.shields.io/badge/Built-for_makers-f97316?style=flat-square" alt="Built for makers"></a>
  <a href="/LICENSE"><img src="https://img.shields.io/badge/License-MIT-64748b?style=flat-square" alt="MIT License"></a>
</p>

<p align="center">
  <a href="https://ai-passport.folotoy.cn/en/">Website</a> ·
  <a href="#start-development-with-one-requirement">Start building</a> ·
  <a href="/docs/reference/README.md">Community projects</a> ·
  <a href="#documentation-index">Documentation</a>
</p>

---

**FoloToy AI Passport** is an open wearable AI platform made for people to shape,
remix, and create. Start with a simple idea, build your own experience, and make
it anything—from a pocket companion to something no one has imagined yet.

<p align="center">
  <img src="../assets/images/home.jpg" alt="FoloToy AI Passport wearable device shown from the front, side, and back." width="100%">
</p>

| Open and remixable | Easy to start | Yours to create |
| --- | --- | --- |
| Open firmware and reusable examples give you room to shape your own experience. | Start from an idea and follow clear guides to make it real, even if this is your first build. | Make a companion, a tool, a game—or anything you can imagine. |

## Find your starting point

| I want to… | Start here |
| --- | --- |
| Use the device or try an official play | [Getting started](https://ai-passport.folotoy.cn/guides/getting-started/) · [Official plays](https://ai-passport.folotoy.cn/plays/) |
| Build a custom application with AI | [Agent instructions](../AGENTS.md) · [AI development guide](development/ai-guide.md) · [Required skills](../skills/README.md) |
| Prepare my environment and build firmware | [Environment setup](/docs/development/engineering/environment-setup.md) · [Build and test](development/engineering/build-and-test.md) |
| Explore the board or contribute | [Hardware guide](hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) · [Contributing](../.github/CONTRIBUTING.md) |

> [!IMPORTANT]
> This fork's `main` carries the **Server-M server-monitoring application**, built on
> the FoloToy AI Passport board support package. Applications derived from this
> repository must still design their own UI; the application screens here are not a
> reusable interface shell. BSP APIs and non-UI logic remain reusable.

## Start development with one requirement

1. Open the repository in your AI coding tool and have it read [`AGENTS.md`](../AGENTS.md).
2. Let it check and install the [five required skills](../skills/README.md), with any permissions your environment requires.
3. Describe what you want to build. Start from `main` on a new `feature/*` branch.

Copy this prompt and adapt it to your idea:

```text
Build an offline habit-tracking application for FoloToy AI Passport.
Use the three physical buttons and the 240×320 display, and preserve records across power loss.
Start from `main`, create a `feature/*` branch, and develop the application there.
Follow AGENTS.md and docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md.
Inspect relevant docs/reference/ application archives first.
Keep hardware logic in components/bsp and application logic in main.
Deliver a runnable implementation with tests; report build results,
unexecuted device checks, and on-device acceptance steps separately.
Redesign the application's UI; do not reuse another application's screens or visual shell.
```

Before starting, check [`docs/reference/`](reference/README.md) for an existing or
reference application and previously recorded, reusable experience. See what is
already built and reusable.

<details>
<summary><strong>Write a clearer requirement</strong> — pages, controls, data, and acceptance</summary>

The more specific the requirement, the more likely the assistant is to implement it correctly in one pass. Useful details include:

- User flow: what each page displays and what short press, double press, and long press do for each button.
- State and data: whether the application needs timing, persistence across power loss, networking, recording, or communication with a computer.
- Experience goals: fonts, colors, animation, sound, response time, and error states.
- Constraints: application navigation and controls, permitted dependencies, and Flash/data usage.
- Acceptance criteria: which behaviors require automated tests and which must be observed on real hardware.

When details are omitted, the assistant may choose conservative defaults that do not change the product direction, but it must list those assumptions in the delivery. Decisions involving new wiring, electrical safety, board revisions, or irreversible data formats require confirmation first.

</details>

> [!NOTE]
> A successful build is not hardware validation. Test the application on a real
> device after implementation; flashing requires your approval. Deliver a verified
> merged `full.bin` for flashing at `0x0`. No original-firmware backup is required,
> but a merged flash may reset stored data. See the [flashing policy](development/engineering/firmware-layout.md#flashing-and-stored-data).

## Application UI is not a template shell

The Server-M application owns its screens, interaction flow, and visual shell. Every derivative application must redesign and implement its own UI; reusing another application's screens or visual shell is prohibited. Renaming or recoloring them does not satisfy this requirement. BSP APIs, ordinary LVGL widgets, lifecycle patterns, and isolated logic may still be reused. See the [mandatory UI redesign rule](development/ai-guide.md#mandatory-ui-redesign-for-derivative-applications).

Start a new application on a `feature/*` branch so the current `main` application stays stable:

```bash
git switch main
git switch -c feature/my-passport-app
```

## Hardware capability contract

**ESP32-C3 · 8 MB Flash · no PSRAM · 240 × 320 display · three physical buttons**

The default layout contains only **NVS, PHY data, and one factory application**
spanning the remaining Flash. User firmware may use another valid 8 MB layout.
See [firmware layout](development/engineering/firmware-layout.md).

<details>
<summary><strong>Expand the capability table</strong> — interfaces, limits, and implementation details</summary>

The table below describes the application capabilities implemented by the current `main` branch. It is not a list of everything that might be possible according to the chip datasheet.

| Capability | Confirmed implementation | Application interface | Boundaries that must be respected |
| --- | --- | --- | --- |
| Display | ST7789P3, 240 × 320 portrait RGB565, SPI2 at 40 MHz; LEDC backlight | `bsp_display_*`, `bsp_lvgl_*` | The ESP32-C3 has no PSRAM; the current design uses a small single DMA buffer; the BSP exposes no LCD MISO, touch, or TE interface |
| Input | `UP`, `DOWN`, and `OK` share an ADC resistor ladder on GPIO0 | `bsp_button_init()`, `bsp_button_read_mv()` | Callbacks run in the button component task and must not block; do not create a second ADC1 unit |
| Audio | ES8311 with full-duplex PCM over I2S0, supporting playback, microphone capture, and software suspend/resume | `bsp_audio_*` | PCM reads and writes block and belong in a worker task; stop PCM I/O before codec sleep; format changes must retain the BSP close/open sequence |
| Battery | CW2017 state-of-charge and voltage readings | `bsp_battery_*` | This capability is optional at runtime; accuracy depends on the cell and battery profile and is not equivalent to a calibrated result |
| Wi-Fi | 2.4 GHz STA connection with saved networks and an on-device provisioning portal | `main/app_wifi.c`, `main/app_portal.c` | Up to four saved networks; the portal serves `192.168.4.1` while the access point is open and the device's LAN IP when already connected; credentials stay in NVS |
| Low power | Backlight-off screen idle with a periodic keep-alive request | `main/main.c` | The application does not enter light or deep sleep; the BSP still exposes terminal deep-sleep helpers, but calling them ends the application |
| Shared bus | ES8311 and CW2017 share I2C0 | `bsp_i2c_*` | Every device must reuse the bus owned by the BSP; do not create another bus on the same port for scanning or a new device |
| Logging and flashing | Native ESP32-C3 USB Serial/JTAG | ESP-IDF console | GPIO18/19 are reserved for USB; the default UART0 TX on GPIO21 conflicts with the backlight |

All pins, addresses, panel parameters, and button voltage windows are defined only in [`components/bsp/include/bsp_pins.h`](../components/bsp/include/bsp_pins.h). Application code must not duplicate these constants. See the [AI Hardware Development Guide](hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.md) for the complete pin map, panel initialization, ADC thresholds, I2C addressing rules, audio clocks, and memory details.

Applications may also use ESP-IDF timers, FreeRTOS tasks, and internal Flash/NVS; `main/app_config.c` shows the current NVS persistence pattern. Wi-Fi remains an ESP-IDF application service rather than a BSP API: `main/app_wifi.c` owns the connection state machine and `main/app_portal.c` owns the temporary HTTP provisioning server. This firmware does not enable Bluetooth.

### Capabilities outside the current contract

The public firmware contract is limited to the interfaces listed above. Do not infer additional board interfaces from the ESP32-C3 feature list. New hardware interfaces require an explicit BSP definition and on-device acceptance criteria.

</details>

## Project structure

Board support lives in `components/bsp`; application pages, state, and tasks live
in `main`. Keep that boundary when building your own firmware.

<details>
<summary><strong>Browse the repository map</strong></summary>

```text
components/bsp/include/  Public BSP APIs and bsp_pins.h hardware facts
components/bsp/src/      Display, button, audio, battery, and shared-I2C implementations
main/                    Server-M application: LVGL UI, Wi-Fi state, HTTP client, and provisioning portal
tests/                   Lightweight logic tests that can run without hardware
tools/                   Shared local/CI validation and firmware verification scripts
docs/                    Project docs, changelog, engineering/contribution rules, and design references
.github/                 GitHub community files, PR template, issue forms, and CI workflows
sdkconfig.defaults       ESP32-C3, USB console, Flash, and LVGL defaults
partitions.csv           Minimal default: NVS, PHY data, and one factory application
dependencies.lock        Reproducible ESP-IDF Managed Component resolution
AGENTS.md                Mandatory AI-agent entry point (paired with AGENTS.zh_CN.md)
CLAUDE.md                Claude Code pointer to AGENTS.md (paired Chinese version)
LICENSE                  Repository license
```

</details>

## Documentation index

Engineering and contribution guides define the rules; examples and archives
provide reference material. Choose the entry that matches your task.

| Resource | What you will find |
| --- | --- |
| [Development](development/README.md) | AI workflow, engineering conventions, CI, and release guidance |
| [AI skills](../skills/README.md) | Development, environment setup, builds, device testing, and debugging |
| [Hardware](hardware-design/README.md) | Board facts, interface boundaries, acceptance checklists, and troubleshooting |
| [Chinese fonts](development/engineering/lvgl-chinese-fonts.md) | Glyph coverage, widget font selection, and blank-text troubleshooting |
| [Wi-Fi provisioning](development/engineering/wifi-provisioning.md) | On-device HTTP portal flow, endpoints, and acceptance checks |
| [Community projects and experience](reference/README.md) | Playbooks and reusable knowledge under `docs/reference/<username>/` |
| [Contributing](contribution/README.md) | Documentation, commits, and pull-request conventions |
| [Brand assets](brand/README.md) | Product visual references and [brand language](brand/brand-and-product.md) |
| [Fork guide](fork-guide.md) · [Changelog](CHANGELOG.md) | Downstream workflows and release history |

---

[Contribute](../.github/CONTRIBUTING.md) · [Get help](../.github/SUPPORT.md) · [Code of conduct](../.github/CODE_OF_CONDUCT.md) · [Security](../.github/SECURITY.md) · [MIT License](../LICENSE)

AI agents: start with [`AGENTS.md`](../AGENTS.md) and follow its task-specific routing.
