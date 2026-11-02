<p align="right">
  <a href="wifi-provisioning.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Wi-Fi Provisioning via the On-Device Portal

The firmware stores up to four Wi-Fi networks in NVS and connects to the active
one at boot. A new network is added from the device: enter the settings page, choose
**Wi-Fi network**, then **Phone web provisioning**. The
device opens a temporary access point and serves an HTTP configuration page; the
phone never needs a companion app or Bluetooth.

## Portal flow

1. The screen shows the access-point name (`AI-Passport-XXXX`, derived from the
   SoftAP MAC) and an eight-character WPA2 password generated randomly at boot.
   The password is only visible on the device because the portal stores server
   API tokens.
2. The phone joins that network and opens `http://192.168.4.1`.
3. The page is ordered **Server → Display settings → Wi-Fi**. Server entries
   carry the relay URL and `X-Api-Token`; display settings carry brightness,
   refresh interval, and screen-off delay.
4. The last card is **Connect and exit provisioning**. Submitting it saves the
   configuration, shows a completion note with the new network name, closes the
   portal, switches the radio back to station mode, and connects.
5. If the device was already online, the portal does not open a second access
   point; it serves the page at the device's LAN address instead. The device
   screen shows that address under **Settings → Wi-Fi network**.

The Wi-Fi card may be skipped. **Exit provisioning without connecting** closes
the portal and returns to the settings screen without changing the active
network.

## Code entry points

- `main/app_portal.c` — HTTP server lifecycle, request handlers, saved-config
  application, and the `then_exit` request that closes the access point before
  reconnecting.
- `main/portal.html` — the embedded page and its JavaScript; it is registered as
  an `EMBED_FILES` entry in `main/CMakeLists.txt`.
- `main/app_wifi.c` — saved-network state machine, scan helper, SoftAP start/stop,
  power save, and reconnect policy.
- `main/app_config.c` — NVS-backed configuration with a snapshot lock shared with
  the UI task.
- `main/main.c` — `exit_portal()` orders the shutdown: return to the local screen,
  stop `httpd`, leave SoftAP mode, then call `app_wifi_note_config_changed()`.

## HTTP endpoints

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/` | Embedded configuration page |
| `GET` | `/api/config` | Current servers, display settings, and saved networks |
| `GET` | `/api/scan` | 2.4 GHz scan result, strongest first |
| `POST` | `/api/wifi` | Add, activate, or delete a saved network |
| `POST` | `/api/server` | Add, edit, or delete a server entry |
| `POST` | `/api/settings` | Brightness, refresh interval, and screen-off delay |
| `POST` | `/api/test` | Test a relay URL and token from the device |
| `POST` | `/api/exit` | Close the portal without changing the active network |

The `httpd` task uses a 20 KB stack because `/api/test` performs a synchronous
TLS request and configuration snapshots occupy stack space. Starting the portal
cancels any in-flight monitoring request first; TLS handshake buffers and the
HTTP server otherwise compete for the same internal RAM.

## Integration rules

- Keep the portal in the application layer, not in `components/bsp`; the BSP
  exposes no networking API.
- Never log or commit Wi-Fi passwords or server tokens. The access-point
  password is generated per boot and displayed only on the device.
- The page uses plain HTTP on the local link because the device has no
  certificate for `192.168.4.1`; this is not a production authorization design.
  Treat the access-point password as the only access control while it is open.
- Do not erase NVS to recover from a corrupted configuration; validate and
  report the error instead.

## Acceptance

Run the [validation gate](build-and-test.md), then follow the
[on-device testing handoff](../ai-guide.md#offer-on-device-testing). On hardware:

- The access-point name and password on screen match what the phone sees.
- `http://192.168.4.1` loads with the server, display, and Wi-Fi cards in that
  order.
- Saving a server and display settings, then connecting Wi-Fi, closes the portal
  and joins the network without a second manual reconnect.
- The new IP appears under **Settings → Wi-Fi network** after the hotspot closes.
- A wrong password leaves the device retrying and does not report success.
- Exiting provisioning without a network returns to settings and keeps the
  previously active network.
- Reboot, screen-off/wake, and repeated portal entry/exit do not duplicate
  `httpd` tasks or leak heap.

Build success does not prove portal compatibility or on-device networking; keep
unperformed checks under `Unverified`.
