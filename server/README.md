<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# AI Passport Server Agent

A single-file relay that runs on every monitored server, calls the 1Panel or
Baota (aaPanel) API on your behalf, and exposes one flat JSON endpoint for the
AI Passport device. The device only performs a plain authenticated `GET`.

## Why a relay

1Panel and Baota sign every request with an API key and a timestamp. The relay
keeps that logic on the server, so the ESP32 never stores panel keys, never
needs the panel port open to the internet, and never parses panel-specific
responses.

```text
ESP32 ── HTTPS ──> Caddy/Nginx ── HTTP ──> 127.0.0.1:8899 ──> 1Panel / Baota API
                                                     └────> /proc, /sys fallback
```

The relay also samples local metrics every `trend_step / 5` seconds and keeps a
24-hour in-memory trend, persisted to `/var/lib/ai-passport/trend.json`. The
trend keeps accumulating while the device screen is off.

## Requirements

- Linux with Python 3.6 or newer (the 3.6.8 shipped with CentOS 7 works).
- `systemd` recommended; the HTTP server still runs without it.
- Optional: 1Panel 2.x or Baota 9.x with its API enabled.
- No third-party Python packages.

## Install

Copy `ai_passport_agent.py` to the server and run it as root:

```bash
sudo python3 ai_passport_agent.py install
```

Without arguments the agent listens on `127.0.0.1:8899`, detects the panel,
uses `/proc` metrics, and generates a random access token. The token is printed
once and stored in `/etc/ai-passport/agent.json` with mode `0600`.

### 1Panel

Enable the API in the panel UI, then:

```bash
sudo python3 ai_passport_agent.py install \
    --panel 1panel \
    --api-key '<API key>' \
    --port 8899
```

The key comes from the panel API settings page. The agent calls
`GET /api/v2/dashboard/current/all/all` with `1Panel-Token` and
`1Panel-Timestamp` headers.

### Baota

```bash
sudo python3 ai_passport_agent.py install \
    --panel baota \
    --api-key '<API key>' \
    --ssl-host example.com
```

The key comes from **Panel settings → API interface**. The agent posts
`GetSystemTotal`, `GetDiskInfo`, and `GetNetWorkApi` to `/system`.
`--ssl-host` is optional and only enables the certificate-expiry field.

If the panel is not on loopback, add `--panel-url http://10.0.0.5:8090`.

### Manual mode

`--panel local` skips the panel entirely and reports `/proc` metrics only.

## GUI install (1Panel / Baota)

You can finish the whole install with the panel's own **file manager, terminal,
and website reverse proxy**; no hand-written nginx config is needed.

| Step | 1Panel | Baota |
| --- | --- | --- |
| 1. Upload the script | Hosts → Files → open `/opt` → create `ai-passport` → upload `ai_passport_agent.py` | Files → open `/opt` → create `ai-passport` → upload `ai_passport_agent.py` |
| 2. Install | Hosts → Terminal, paste the 1Panel command below | Terminal, paste the Baota command below |
| 3. Check liveness | `curl -s http://127.0.0.1:8899/healthz` returns `{"ok":true,...}` | Same |
| 4. Create a site | Websites → Create → type "static", domain `monitor.example.com` | Websites → Add site → PHP "static", domain `monitor.example.com` |
| 5. Reverse proxy | Site settings → Reverse proxy → add: target `http://127.0.0.1:8899` | Settings → Reverse proxy → add: target URL `http://127.0.0.1:8899`, send domain `$host`, cache off |
| 6. Certificate | Websites → HTTPS → request Let's Encrypt → enable "force HTTPS" | SSL → Let's Encrypt → force HTTPS |
| 7. Open ports | Hosts → Firewall, allow 80 and 443 | Security, allow 80 and 443 |

The step 2 commands (replace the key with the one copied from the panel):

```bash
# 1Panel (key: Panel settings → API interface)
sudo python3 /opt/ai-passport/ai_passport_agent.py install \
    --panel 1panel --api-key '<API key>'

# Baota (key: Panel settings → API interface)
sudo python3 /opt/ai-passport/ai_passport_agent.py install \
    --panel baota --api-key '<API key>'
```

The installer creates and starts the `ai-passport-agent` systemd service,
generates a random access token, and writes it to
`/etc/ai-passport/agent.json` with mode `0600`.

If the panel has no terminal entry, run the same command over SSH; it works
from any directory because the installer uses absolute paths.

Opening `https://monitor.example.com/status` in a browser shows
`{"ok":false,"error":"unauthorized"}`. That is **expected**: browsers do not
send `X-Api-Token`. Verify from a terminal instead:

```bash
TOKEN=$(sudo python3 -c 'import json;print(json.load(open("/etc/ai-passport/agent.json"))["token"])')
curl -s -H "X-Api-Token: $TOKEN" http://127.0.0.1:8899/status
```

Use `https://monitor.example.com/status` and the same token in the device
portal.

GUI uninstall: run
`sudo python3 /opt/ai-passport/ai_passport_agent.py uninstall --purge` in the
panel terminal, then delete the site from the website list.

## Publish over HTTPS

Keep the agent bound to loopback and terminate TLS in front of it. Example
Caddyfile (also see [Caddyfile.example](Caddyfile.example)):

```caddyfile
monitor.example.com {
    reverse_proxy 127.0.0.1:8899
}
```

Then use `https://monitor.example.com/status` in the device portal. The device
verifies certificates against the ESP-IDF CA bundle, so a public CA certificate
is required; self-signed certificates are rejected.

## Endpoint

`GET /status` with header `X-Api-Token: <token>` returns:

```json
{
  "online": true,
  "ts": 1760000000,
  "panel": "1Panel",
  "cpu": 12,
  "mem": 63,
  "swap": 0,
  "disk": 88,
  "load1": 0.42,
  "net_rx": 2048,
  "net_tx": 1024,
  "disk_read": 4096,
  "disk_write": 2048,
  "uptime": 86400,
  "temp": 46.5,
  "docker_run": 7,
  "docker_total": 8,
  "docker_bad": 1,
  "ssl_days": 42,
  "trend": {"step": 300, "cpu": [12, 15], "mem": [63, 64]}
}
```

| Field | Meaning |
| --- | --- |
| `cpu`, `mem`, `swap`, `disk` | Percent, 0-100. Missing values are `-1`. |
| `load1` | 1-minute load average. |
| `net_rx`, `net_tx` | Bytes per second over the sampling window. |
| `disk_read`, `disk_write` | Whole-disk read/write bytes per second over the sampling window. |
| `uptime` | Seconds since boot. |
| `temp` | Celsius, when a thermal zone or panel value exists. |
| `docker_run`, `docker_total`, `docker_bad` | Running, all, and abnormal (not `running`) containers, when Docker is present. |
| `ssl_days` | Days until `ssl_host` expires; `-1` when disabled or unreachable. |
| `trend.step` | Seconds per trend point. |
| `trend.cpu`, `trend.mem` | Up to 288 samples, oldest first. |
| `warn` | Present only when a panel call failed and local metrics were used. |

`GET /healthz` is unauthenticated and returns `{"ok": true, "version": "..."}`
for monitoring probes. Wrong or missing tokens receive `401`.

## Verify

```bash
sudo python3 /opt/ai-passport/ai_passport_agent.py test
curl -H "X-Api-Token: $(sudo python3 -c 'import json;print(json.load(open("/etc/ai-passport/agent.json"))["token"])')" \
     http://127.0.0.1:8899/status
```

`test` exits non-zero when the panel call fails or CPU/memory are unavailable.

## Operate

```bash
sudo systemctl status ai-passport-agent
sudo journalctl -u ai-passport-agent -n 50
```

Config changes: edit `/etc/ai-passport/agent.json`, then
`sudo systemctl restart ai-passport-agent`. To change the panel key, rerun
`install` with the same arguments; it keeps the existing token and trend data.

## Uninstall

```bash
sudo python3 ai_passport_agent.py uninstall             # keep /etc/ai-passport
sudo python3 ai_passport_agent.py uninstall --purge     # also delete trend data
```

The uninstaller stops and disables the service, removes the unit file,
`/opt/ai-passport`, and `/etc/ai-passport` unless `--keep-config` is given.

## Troubleshooting

- **`401 unauthorized`**: token mismatch. Recheck the token in the device
  portal, then the `X-Api-Token` value in `agent.json`.
- **`panel` reports the local fallback and `warn` is set**: the panel key or URL
  is wrong, or the panel API is disabled. Run `test` to see the exact HTTP
  error.
- **`cpu` or `mem` is `-1`**: the panel response lacked the field; the agent
  falls back to `/proc`, which needs one sampling interval after startup.
- **Device reports SNTP errors**: TLS needs a valid clock. Allow UDP 123 out,
  or point the device at a reachable NTP server.
- **Certificate errors on the device**: use a public CA. Browsers may accept a
  self-signed certificate that the device will reject.

## Security

- Bind to `127.0.0.1` unless you have a trusted private network.
- The token is the only access control on `/status`; keep it out of screenshots
  and logs. Rotate it by editing `agent.json` and restarting.
- Panel keys stay in `agent.json` (mode `0600`); they are never sent to the
  device.
- The agent calls the panel over HTTP by default because it runs on the same
  host. It skips certificate verification for those local calls; do not point
  `--panel-url` at an untrusted remote host.
