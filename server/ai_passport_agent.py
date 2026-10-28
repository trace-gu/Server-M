#!/usr/bin/env python3
"""AI Passport 服务器端状态中转脚本。

用途：在每台被监控的服务器上运行本脚本。它读取 1Panel / 宝塔面板的官方 API，
并补充 /proc、/sys 中的通用指标，输出一个极简 JSON 接口供 ESP32 直接 GET：

    GET http://127.0.0.1:8899/status
    X-Api-Token: <安装时生成的 Token>

特点：
* 仅使用 Python 3 标准库，无第三方依赖；
* 面板 API 失败时自动回退到 /proc 本机指标，屏幕不会因此空白；
* 24 小时迷你趋势在服务器侧持续采样，ESP32 熄屏也不影响历史积累；
* 自带 install / uninstall / test / status / run 子命令。
"""

from __future__ import annotations

import argparse
import hashlib
import hmac
import json
import os
import re
import shutil
import socket
import ssl
import subprocess
import sys
import threading
import time
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from urllib import error as urlerror
from urllib import parse as urlparse
from urllib import request as urlrequest

VERSION = "1.0.0"

INSTALL_DIR = Path("/opt/ai-passport")
CONFIG_DIR = Path("/etc/ai-passport")
CONFIG_PATH = CONFIG_DIR / "agent.json"
STATE_DIR = Path("/var/lib/ai-passport")
TREND_PATH = STATE_DIR / "trend.json"
SERVICE_PATH = Path("/etc/systemd/system/ai-passport-agent.service")

DEFAULT_BIND = "127.0.0.1"
DEFAULT_PORT = 8899
DEFAULT_TREND_STEP = 300
TREND_POINTS = 288

PANEL_PORTS = {"1panel": 8090, "baota": 8888}


# ---------------------------------------------------------------------------
# 配置
# ---------------------------------------------------------------------------
def default_config() -> dict:
    return {
        "bind": DEFAULT_BIND,
        "port": DEFAULT_PORT,
        "token": "",
        "panel": "auto",          # auto | 1panel | baota | local
        "panel_url": "",
        "panel_key": "",
        "ssl_host": "",           # 可选: 如 example.com 或 example.com:8443
        "trend_step": DEFAULT_TREND_STEP,
    }


def load_config(path: Path = CONFIG_PATH) -> dict:
    cfg = default_config()
    try:
        with open(path, "r", encoding="utf-8") as fh:
            stored = json.load(fh)
    except FileNotFoundError:
        return cfg
    except (OSError, ValueError) as exc:
        raise SystemExit(f"读取配置失败 {path}: {exc}")
    if isinstance(stored, dict):
        cfg.update({k: v for k, v in stored.items() if v is not None})
    cfg["port"] = int(cfg.get("port") or DEFAULT_PORT)
    cfg["trend_step"] = max(60, int(cfg.get("trend_step") or DEFAULT_TREND_STEP))
    return cfg


def save_config(cfg: dict, path: Path = CONFIG_PATH) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(".tmp")
    with open(tmp, "w", encoding="utf-8") as fh:
        json.dump(cfg, fh, ensure_ascii=False, indent=2)
        fh.write("\n")
    os.chmod(tmp, 0o600)
    os.replace(tmp, path)


# ---------------------------------------------------------------------------
# 面板签名与响应解析（纯函数，便于单测）
# ---------------------------------------------------------------------------
def token_1panel(api_key: str, timestamp: str) -> str:
    """1Panel: md5('1panel' + API-Key + UnixTimestamp)。"""
    return hashlib.md5(f"1panel{api_key}{timestamp}".encode("utf-8")).hexdigest()


def sign_baota(api_key: str, timestamp: str) -> str:
    """宝塔: md5(request_time + md5(api_sk))。"""
    key_md5 = hashlib.md5(api_key.encode("utf-8")).hexdigest()
    return hashlib.md5((timestamp + key_md5).encode("utf-8")).hexdigest()


def _clamp_percent(value) -> int:
    """把 0-100 的百分比取整；负值表示“面板没有提供”，保持 -1。"""
    try:
        number = float(value)
    except (TypeError, ValueError):
        return -1
    if number < 0:
        return -1
    return int(round(min(100.0, number)))


def _percent(used, total) -> int:
    try:
        used = float(used)
        total = float(total)
    except (TypeError, ValueError):
        return -1
    if total <= 0:
        return -1
    return _clamp_percent(used / total * 100.0)


def _as_float(value, fallback=None):
    try:
        return float(value)
    except (TypeError, ValueError):
        return fallback


def parse_1panel(payload: dict) -> dict:
    """把 1Panel 的 DashboardBase / DashboardCurrent 响应映射成统一指标。"""
    out: dict = {"panel": "1Panel"}
    if not isinstance(payload, dict):
        return out
    data = payload.get("data") if isinstance(payload.get("data"), dict) else payload
    cur = data.get("currentInfo") if isinstance(data.get("currentInfo"), dict) else data

    out["cpu"] = _clamp_percent(cur.get("cpuUsedPercent", -1))
    out["mem"] = _clamp_percent(cur.get("memoryUsedPercent", -1))
    out["swap"] = _clamp_percent(cur.get("swapMemoryUsedPercent", -1))
    load1 = _as_float(cur.get("load1"))
    if load1 is not None:
        out["load1"] = round(load1, 2)
    uptime = _as_float(cur.get("uptime"))
    if uptime is not None and uptime >= 0:
        out["uptime"] = int(uptime)

    disks = cur.get("diskData")
    out["disk"] = -1
    if isinstance(disks, list) and disks:
        values = [_clamp_percent(item.get("usedPercent")) for item in disks
                  if isinstance(item, dict) and item.get("usedPercent") is not None]
        if values:
            out["disk"] = max(values)
    return out


def parse_baota(total: dict, disk, network: dict, load: dict) -> dict:
    """把宝塔 GetSystemTotal / GetDiskInfo / GetNetWorkApi / GetLoadAverage 合并。"""
    out: dict = {"panel": "宝塔", "cpu": -1, "mem": -1, "disk": -1}
    if isinstance(total, dict):
        mem_total = _as_float(total.get("memTotal"))
        mem_used = _as_float(total.get("memRealUsed"))
        if mem_used is not None:
            out["mem"] = _percent(mem_used, mem_total)
        out["cpu"] = _clamp_percent(total.get("cpuRealUsed", -1))
        version = total.get("version")
        if isinstance(version, str) and version:
            out["panel_version"] = version[:15]

    if isinstance(disk, list):
        percents = []
        for item in disk:
            if not isinstance(item, dict):
                continue
            sizes = item.get("size")
            if isinstance(sizes, list) and len(sizes) >= 4:
                try:
                    percents.append(_clamp_percent(float(str(sizes[3]).rstrip("%"))))
                except ValueError:
                    pass
            byte_size = item.get("byte_size")
            if isinstance(byte_size, list) and len(byte_size) >= 2:
                percents.append(_percent(byte_size[1], byte_size[0]))
        percents = [p for p in percents if p >= 0]
        if percents:
            out["disk"] = max(percents)

    if isinstance(network, dict):
        down = _as_float(network.get("down"))
        up = _as_float(network.get("up"))
        if down is not None and down >= 0:
            out["net_rx"] = int(down * 1024)      # 宝塔单位 KB/s
        if up is not None and up >= 0:
            out["net_tx"] = int(up * 1024)
        load_inner = network.get("load")
        if isinstance(load_inner, dict):
            one = _as_float(load_inner.get("one"))
            if one is not None:
                out["load1"] = round(one, 2)

    if isinstance(load, dict):
        one = _as_float(load.get("one"))
        if one is not None:
            out["load1"] = round(one, 2)
    return out


# ---------------------------------------------------------------------------
# 本机 /proc、/sys 指标
# ---------------------------------------------------------------------------
def read_cpu_times() -> tuple[int, int] | None:
    try:
        with open("/proc/stat", "r", encoding="ascii") as fh:
            line = fh.readline()
    except OSError:
        return None
    parts = line.split()
    if len(parts) < 5 or parts[0] != "cpu":
        return None
    values = [int(v) for v in parts[1:]]
    idle = values[3] + (values[4] if len(values) > 4 else 0)
    return sum(values), idle


def read_meminfo() -> dict:
    info: dict[str, int] = {}
    try:
        with open("/proc/meminfo", "r", encoding="ascii") as fh:
            for line in fh:
                key, _, rest = line.partition(":")
                parts = rest.split()
                if parts:
                    info[key] = int(parts[0])
    except OSError:
        return {}
    return info


def read_net_bytes() -> tuple[int, int] | None:
    try:
        with open("/proc/net/dev", "r", encoding="ascii") as fh:
            lines = fh.readlines()[2:]
    except OSError:
        return None
    rx = tx = 0
    for line in lines:
        name, _, rest = line.partition(":")
        if name.strip() == "lo":
            continue
        fields = rest.split()
        if len(fields) < 9:
            continue
        rx += int(fields[0])
        tx += int(fields[8])
    return rx, tx


def _base_device(name: str) -> str:
    """把分区名归一成整盘名：sda1->sda, nvme0n1p1->nvme0n1, mmcblk0p1->mmcblk0。"""
    if name.startswith(("nvme", "mmcblk", "md")):
        return re.sub(r"p\d+$", "", name)
    return re.sub(r"\d+$", "", name)


def read_disk_io() -> tuple[int, int] | None:
    """返回 (累计读字节, 累计写字节)。只统计整块物理盘,避免分区重复计数。"""
    skip = ("loop", "ram", "zram", "dm-", "md", "sr", "fd")
    try:
        with open("/proc/diskstats", "r", encoding="ascii") as fh:
            lines = fh.readlines()
    except OSError:
        return None
    totals: dict[str, tuple[int, int]] = {}
    for line in lines:
        parts = line.split()
        if len(parts) < 14:
            continue
        name = parts[2]
        if name.startswith(skip) or _base_device(name) != name:
            continue
        try:
            totals[name] = (int(parts[5]) * 512, int(parts[9]) * 512)
        except ValueError:
            continue
    if not totals:
        return None
    return (sum(v[0] for v in totals.values()),
            sum(v[1] for v in totals.values()))


def read_uptime() -> int:
    try:
        with open("/proc/uptime", "r", encoding="ascii") as fh:
            return int(float(fh.read().split()[0]))
    except (OSError, ValueError, IndexError):
        return 0


def read_temperature() -> float | None:
    best = None
    for path in Path("/sys/class/thermal").glob("thermal_zone*/temp"):
        try:
            value = float(path.read_text(encoding="ascii").strip()) / 1000.0
        except (OSError, ValueError):
            continue
        if 0 < value < 200 and (best is None or value > best):
            best = value
    return best


def read_disk_percent() -> int:
    """取真实挂载点中最高的使用率；伪文件系统会被跳过。"""
    skip = {"proc", "sysfs", "tmpfs", "devtmpfs", "devpts", "cgroup",
            "cgroup2", "overlay", "squashfs", "ramfs", "autofs", "nsfs",
            "mqueue", "debugfs", "tracefs", "securityfs", "pstore", "bpf",
            "hugetlbfs", "configfs", "fusectl", "binfmt_misc", "efivarfs"}
    best = -1
    try:
        with open("/proc/mounts", "r", encoding="utf-8") as fh:
            mounts = [line.split() for line in fh if len(line.split()) >= 3]
    except OSError:
        mounts = []
    seen = set()
    for parts in mounts:
        device, mountpoint, fstype = parts[0], parts[1], parts[2]
        if fstype in skip or not device.startswith("/dev/"):
            continue
        if mountpoint in seen:
            continue
        seen.add(mountpoint)
        try:
            stat = os.statvfs(mountpoint)
        except OSError:
            continue
        total = stat.f_blocks * stat.f_frsize
        free = stat.f_bavail * stat.f_frsize
        if total <= 0:
            continue
        percent = _percent(total - free, total)
        if percent > best:
            best = percent
    if best < 0:
        try:
            stat = os.statvfs("/")
            best = _percent(stat.f_blocks - stat.f_bavail, stat.f_blocks)
        except OSError:
            best = -1
    return best


class LocalMetrics:
    """线程安全的本机指标采样器，同时维护 24 小时趋势。"""

    def __init__(self, trend_step: int):
        self._lock = threading.Lock()
        self._prev_cpu = None
        self._prev_net = None
        self._prev_disk = None
        self._prev_time = None
        self._cpu = -1
        self._rx_bps = 0
        self._tx_bps = 0
        self._disk_r_bps = 0
        self._disk_w_bps = 0
        self._trend_step = max(60, int(trend_step))
        self._trend: deque[list] = deque(maxlen=TREND_POINTS)
        self._last_trend_ts = 0.0
        self._load_trend()
        self._sample(force=True)

    # ---- 趋势持久化 ----
    def _load_trend(self) -> None:
        try:
            with open(TREND_PATH, "r", encoding="utf-8") as fh:
                stored = json.load(fh)
        except (OSError, ValueError):
            return
        if not isinstance(stored, list):
            return
        cutoff = time.time() - TREND_POINTS * self._trend_step
        for item in stored[-TREND_POINTS:]:
            if (isinstance(item, list) and len(item) == 3
                    and all(isinstance(v, (int, float)) for v in item)
                    and item[0] >= cutoff):
                self._trend.append([int(item[0]), int(item[1]), int(item[2])])
        if self._trend:
            self._last_trend_ts = self._trend[-1][0]

    def _save_trend(self) -> None:
        try:
            STATE_DIR.mkdir(parents=True, exist_ok=True)
            tmp = TREND_PATH.with_suffix(".tmp")
            with open(tmp, "w", encoding="utf-8") as fh:
                json.dump(list(self._trend), fh, separators=(",", ":"))
            os.replace(tmp, TREND_PATH)
        except OSError:
            pass

    # ---- 采样 ----
    def _sample(self, force: bool = False) -> dict:
        now = time.monotonic()
        wall = time.time()
        with self._lock:
            cpu = read_cpu_times()
            if cpu and self._prev_cpu:
                total_delta = cpu[0] - self._prev_cpu[0]
                idle_delta = cpu[1] - self._prev_cpu[1]
                if total_delta > 0:
                    self._cpu = _clamp_percent((1.0 - idle_delta / total_delta) * 100.0)
            if cpu:
                self._prev_cpu = cpu

            net = read_net_bytes()
            if net and self._prev_net and self._prev_time is not None:
                elapsed = max(0.001, now - self._prev_time)
                self._rx_bps = max(0, int((net[0] - self._prev_net[0]) / elapsed))
                self._tx_bps = max(0, int((net[1] - self._prev_net[1]) / elapsed))
            if net:
                self._prev_net = net

            disk = read_disk_io()
            if disk and self._prev_disk and self._prev_time is not None:
                elapsed = max(0.001, now - self._prev_time)
                self._disk_r_bps = max(0, int((disk[0] - self._prev_disk[0]) / elapsed))
                self._disk_w_bps = max(0, int((disk[1] - self._prev_disk[1]) / elapsed))
            if disk:
                self._prev_disk = disk
            self._prev_time = now

            mem = read_meminfo()
            mem_total = mem.get("MemTotal", 0)
            mem_avail = mem.get("MemAvailable", mem.get("MemFree", 0))
            swap_total = mem.get("SwapTotal", 0)
            swap_free = mem.get("SwapFree", 0)
            value = {
                "cpu": self._cpu,
                "mem": _percent(mem_total - mem_avail, mem_total),
                "swap": _percent(swap_total - swap_free, swap_total) if swap_total else 0,
                "disk": read_disk_percent(),
                "net_rx": self._rx_bps,
                "net_tx": self._tx_bps,
                "disk_read": self._disk_r_bps,
                "disk_write": self._disk_w_bps,
                "uptime": read_uptime(),
            }
            try:
                value["load1"] = round(os.getloadavg()[0], 2)
            except OSError:
                pass
            temp = read_temperature()
            if temp is not None:
                value["temp"] = round(temp, 1)

            now_wall = wall
            if force or now_wall - self._last_trend_ts >= self._trend_step:
                self._last_trend_ts = now_wall
                self._trend.append([int(now_wall), max(0, value["cpu"]), max(0, value["mem"])])
                self._save_trend()
        return value

    def snapshot(self) -> dict:
        return self._sample()

    def trend_payload(self) -> dict:
        with self._lock:
            entries = list(self._trend)
        cutoff = time.time() - TREND_POINTS * self._trend_step
        entries = [e for e in entries if e[0] >= cutoff]
        return {
            "step": self._trend_step,
            "cpu": [max(0, min(100, int(e[1]))) for e in entries],
            "mem": [max(0, min(100, int(e[2]))) for e in entries],
        }

    def sampler_loop(self, stop: threading.Event) -> None:
        interval = max(30, self._trend_step // 5)
        while not stop.wait(interval):
            try:
                self._sample()
            except Exception:
                pass


# ---------------------------------------------------------------------------
# 面板 API 调用
# ---------------------------------------------------------------------------
def _tcp_open(host: str, port: int, timeout: float = 0.4) -> bool:
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def detect_panel(cfg: dict) -> str:
    panel = str(cfg.get("panel") or "auto").strip().lower()
    if panel in ("1panel", "baota"):
        return panel
    if panel in ("local", "none", "off"):
        return "local"
    if cfg.get("panel_url"):
        return "1panel" if ":8090" in cfg["panel_url"] else "baota"
    if _tcp_open("127.0.0.1", PANEL_PORTS["1panel"]):
        return "1panel"
    if _tcp_open("127.0.0.1", PANEL_PORTS["baota"]):
        return "baota"
    return "local"


def panel_base(cfg: dict, panel: str) -> str:
    url = str(cfg.get("panel_url") or "").strip()
    if not url:
        url = f"http://127.0.0.1:{PANEL_PORTS[panel]}"
    return url.rstrip("/")


def _http_json(req: urlrequest.Request, timeout: float = 4.0):
    context = ssl._create_unverified_context()  # 面板通常在本机或内网自签名
    try:
        with urlrequest.urlopen(req, timeout=timeout, context=context) as resp:
            body = resp.read(256 * 1024)
    except urlerror.HTTPError as exc:
        detail = exc.read(512).decode("utf-8", "replace")
        raise RuntimeError(f"HTTP {exc.code}: {detail}") from exc
    except (urlerror.URLError, OSError) as exc:
        raise RuntimeError(str(exc)) from exc
    try:
        return json.loads(body.decode("utf-8"))
    except ValueError as exc:
        raise RuntimeError("面板返回的不是 JSON") from exc


def fetch_1panel(cfg: dict) -> dict:
    key = str(cfg.get("panel_key") or "")
    if not key:
        raise RuntimeError("未配置 panel_key")
    timestamp = str(int(time.time()))
    headers = {
        "1Panel-Token": token_1panel(key, timestamp),
        "1Panel-Timestamp": timestamp,
        "Accept": "application/json",
        "User-Agent": f"AI-Passport-Agent/{VERSION}",
    }
    url = f"{panel_base(cfg, '1panel')}/api/v2/dashboard/current/all/all"
    payload = _http_json(urlrequest.Request(url, headers=headers))
    return parse_1panel(payload)


def _baota_call(cfg: dict, action: str):
    timestamp = str(int(time.time()))
    data = urlparse.urlencode({
        "request_time": timestamp,
        "request_token": sign_baota(str(cfg.get("panel_key") or ""), timestamp),
        "action": action,
    }).encode("utf-8")
    req = urlrequest.Request(
        f"{panel_base(cfg, 'baota')}/system",
        data=data,
        headers={
            "Content-Type": "application/x-www-form-urlencoded",
            "Accept": "application/json",
            "User-Agent": f"AI-Passport-Agent/{VERSION}",
        },
    )
    payload = _http_json(req)
    if isinstance(payload, dict) and payload.get("status") is False:
        raise RuntimeError(str(payload.get("msg") or "面板返回失败"))
    return payload


def fetch_baota(cfg: dict) -> dict:
    if not str(cfg.get("panel_key") or ""):
        raise RuntimeError("未配置 panel_key")
    total = _baota_call(cfg, "GetSystemTotal")
    try:
        disk = _baota_call(cfg, "GetDiskInfo")
        network = _baota_call(cfg, "GetNetWorkApi")
        load = network.get("load") if isinstance(network, dict) else None
    except RuntimeError:
        disk, network, load = None, {}, None
    return parse_baota(total, disk, network, load)


# ---------------------------------------------------------------------------
# 可选采集：Docker / 证书到期
# ---------------------------------------------------------------------------
def docker_counts(cache: dict, lock: threading.Lock) -> tuple[int, int, int]:
    """返回 (运行中数量, 容器总数, 异常数量)。

    设备列表/详情显示 ``运行中/总数``;异常数量只用于诊断。
    """
    now = time.time()
    with lock:
        if cache.get("ts") and now - cache["ts"] < 30:
            return cache.get("run", -1), cache.get("total", -1), cache.get("bad", -1)
    binary = shutil.which("docker")
    run = total = bad = -1
    if binary:
        try:
            out = subprocess.run(
                [binary, "ps", "-a", "--format", "{{.State}}"],
                capture_output=True, text=True, timeout=3,
            )
            if out.returncode == 0:
                states = [line.strip() for line in out.stdout.splitlines() if line.strip()]
                total = len(states)
                run = sum(1 for state in states if state == "running")
                bad = sum(1 for state in states
                          if state in ("exited", "dead", "restarting", "paused", "created"))
        except (OSError, subprocess.SubprocessError):
            pass
    with lock:
        cache.update({"ts": now, "run": run, "total": total, "bad": bad})
    return run, total, bad


def ssl_days_left(host: str, cache: dict, lock: threading.Lock) -> int:
    if not host:
        return -1
    now = time.time()
    with lock:
        if cache.get("ts") and now - cache["ts"] < 3600:
            return cache.get("days", -1)
    hostname, _, port_text = host.partition(":")
    port = int(port_text) if port_text.isdigit() else 443
    days = -1
    try:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        with socket.create_connection((hostname, port), timeout=4) as sock:
            with context.wrap_socket(sock, server_hostname=hostname) as tls:
                cert = tls.getpeercert()
        expires = ssl.cert_time_to_seconds(cert["notAfter"])
        days = max(-1, int((expires - time.time()) // 86400))
    except (OSError, ssl.SSLError, KeyError, ValueError):
        days = -1
    with lock:
        cache.update({"ts": now, "days": days})
    return days


# ---------------------------------------------------------------------------
# 状态组装与 HTTP 服务
# ---------------------------------------------------------------------------
class Agent:
    def __init__(self, cfg: dict, verbose: bool = False):
        self.cfg = cfg
        self.verbose = verbose
        self.local = LocalMetrics(cfg.get("trend_step", DEFAULT_TREND_STEP))
        self._cache_lock = threading.Lock()
        self._docker_cache: dict = {}
        self._ssl_cache: dict = {}
        self._stop = threading.Event()
        self._sampler = threading.Thread(target=self.local.sampler_loop, args=(self._stop,),
                                         name="trend-sampler", daemon=True)
        self.panel = detect_panel(cfg)
        self.warn = ""

    def start(self) -> None:
        self._sampler.start()

    def stop(self) -> None:
        self._stop.set()

    def status(self) -> dict:
        local = self.local.snapshot()
        payload = {
            "online": True,
            "ts": int(time.time()),
            "panel": "本地",
        }
        for key in ("cpu", "mem", "swap", "disk", "load1", "net_rx", "net_tx",
                    "disk_read", "disk_write", "uptime", "temp"):
            if key in local:
                payload[key] = local[key]

        warn = ""
        if self.panel in ("1panel", "baota"):
            try:
                panel_data = fetch_1panel(self.cfg) if self.panel == "1panel" else fetch_baota(self.cfg)
                for key, value in panel_data.items():
                    if key == "panel":
                        payload["panel"] = value
                    elif value is not None and value != -1:
                        payload[key] = value
                # 面板的单位是 KB/s（宝塔），统一覆盖本机值；1Panel 不提供速率。
                if "net_rx" in panel_data:
                    payload["net_rx"] = panel_data["net_rx"]
                if "net_tx" in panel_data:
                    payload["net_tx"] = panel_data["net_tx"]
            except RuntimeError as exc:
                warn = f"{self.panel} API: {exc}"
                payload["panel"] = "本地"
        else:
            payload["panel"] = "本地"

        run, total, bad = docker_counts(self._docker_cache, self._cache_lock)
        if run >= 0:
            payload["docker_run"] = run
            payload["docker_total"] = total
            payload["docker_bad"] = bad
        days = ssl_days_left(str(self.cfg.get("ssl_host") or ""),
                             self._ssl_cache, self._cache_lock)
        if days >= 0:
            payload["ssl_days"] = days
        payload["trend"] = self.local.trend_payload()
        if warn:
            payload["warn"] = warn[:160]
            self.warn = warn
        return payload


class StatusRequestHandler(BaseHTTPRequestHandler):
    server_version = f"AIPassportAgent/{VERSION}"
    protocol_version = "HTTP/1.1"

    def _send_json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Cache-Control", "no-store")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802 - BaseHTTPRequestHandler 约定
        path = urlparse.urlsplit(self.path).path
        if path == "/healthz":
            self._send_json(200, {"ok": True, "version": VERSION})
            return
        if path != "/status":
            self._send_json(404, {"ok": False, "error": "not found"})
            return
        expected = self.server.agent.cfg.get("token") or ""
        supplied = self.headers.get("X-Api-Token", "")
        if not expected or not supplied or not hmac.compare_digest(supplied, expected):
            self._send_json(401, {"ok": False, "error": "unauthorized"})
            return
        try:
            payload = self.server.agent.status()
        except Exception as exc:  # 兜底，避免线程异常导致空连接
            self._send_json(500, {"ok": False, "error": str(exc)[:160]})
            return
        self._send_json(200, payload)

    def log_message(self, fmt: str, *args) -> None:
        if getattr(self.server.agent, "verbose", False):
            sys.stderr.write("%s - %s\n" % (self.address_string(), fmt % args))


class AgentHTTPServer(ThreadingHTTPServer):
    daemon_threads = True
    allow_reuse_address = True

    def __init__(self, address, handler, agent: Agent):
        super().__init__(address, handler)
        self.agent = agent


# ---------------------------------------------------------------------------
# CLI 子命令
# ---------------------------------------------------------------------------
def _require_root() -> None:
    if os.geteuid() != 0:
        raise SystemExit("请用 root 运行（例如 sudo python3 ai_passport_agent.py install ...）")


def _python_path() -> str:
    return shutil.which("python3") or sys.executable


def service_unit(python: str) -> str:
    return f"""[Unit]
Description=AI Passport server status agent
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart={python} {INSTALL_DIR / 'ai_passport_agent.py'} run --config {CONFIG_PATH}
Restart=on-failure
RestartSec=3
NoNewPrivileges=yes

[Install]
WantedBy=multi-user.target
"""


def cmd_install(args) -> int:
    _require_root()
    cfg = load_config() if CONFIG_PATH.exists() else default_config()
    if args.bind:
        cfg["bind"] = args.bind
    if args.port:
        cfg["port"] = int(args.port)
    if args.token:
        cfg["token"] = args.token
    if args.panel:
        cfg["panel"] = args.panel
    if args.panel_url:
        cfg["panel_url"] = args.panel_url
    if args.api_key:
        cfg["panel_key"] = args.api_key
    if args.ssl_host is not None:
        cfg["ssl_host"] = args.ssl_host
    if not cfg.get("token"):
        import secrets
        cfg["token"] = secrets.token_urlsafe(24)

    panel = detect_panel(cfg)
    if panel != "local" and not cfg.get("panel_key"):
        print("提示: 未提供 --api-key，将只使用 /proc 本机指标；面板 API 不会启用。")

    INSTALL_DIR.mkdir(parents=True, exist_ok=True)
    CONFIG_DIR.mkdir(parents=True, exist_ok=True)
    STATE_DIR.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve()
    target = INSTALL_DIR / "ai_passport_agent.py"
    if source != target:
        shutil.copy2(source, target)
    os.chmod(target, 0o755)
    save_config(cfg, CONFIG_PATH)

    systemd_available = shutil.which("systemctl") is not None
    if systemd_available:
        SERVICE_PATH.write_text(service_unit(_python_path()), encoding="utf-8")
        subprocess.run(["systemctl", "daemon-reload"], check=False)
        subprocess.run(["systemctl", "enable", "ai-passport-agent.service"], check=False)
        if not args.no_start:
            result = subprocess.run(["systemctl", "restart", "ai-passport-agent.service"])
            if result.returncode != 0:
                print("服务启动失败，请查看: journalctl -u ai-passport-agent -n 50", file=sys.stderr)
                return 1

    bind = cfg["bind"]
    port = cfg["port"]
    print("AI Passport 服务器端中转已安装。")
    print(f"  监听:     http://{bind}:{port}/status")
    print(f"  访问令牌: {cfg['token']}")
    print(f"  面板模式: {panel}")
    print("  本机验证: curl -H \"X-Api-Token: %s\" http://%s:%d/status" %
          (cfg["token"], bind, port))
    if not systemd_available:
        print("  未检测到 systemd，请手动常驻: %s %s run --config %s" %
              (_python_path(), target, CONFIG_PATH))
    print("下一步: 用 Caddy/Nginx 把本接口反代成 HTTPS，把 https://你的域名/status 和 Token 填入设备配网页。")
    return 0


def cmd_uninstall(args) -> int:
    _require_root()
    if shutil.which("systemctl"):
        subprocess.run(["systemctl", "disable", "--now", "ai-passport-agent.service"], check=False)
        if SERVICE_PATH.exists():
            SERVICE_PATH.unlink()
        subprocess.run(["systemctl", "daemon-reload"], check=False)
    if INSTALL_DIR.exists():
        shutil.rmtree(INSTALL_DIR, ignore_errors=True)
    if not args.keep_config and CONFIG_DIR.exists():
        shutil.rmtree(CONFIG_DIR, ignore_errors=True)
    if args.purge and STATE_DIR.exists():
        shutil.rmtree(STATE_DIR, ignore_errors=True)
    print("已卸载 AI Passport 服务器端中转。")
    if args.keep_config:
        print(f"已保留配置: {CONFIG_PATH}")
    if not args.purge:
        print(f"已保留趋势数据: {TREND_PATH}（--purge 可一并删除）")
    return 0


def cmd_test(args) -> int:
    cfg = load_config(Path(args.config))
    if not cfg.get("token"):
        print("配置中没有 token，请先运行 install 或手动填写。", file=sys.stderr)
        return 1
    agent = Agent(cfg, verbose=True)
    payload = agent.status()
    print(json.dumps(payload, ensure_ascii=False, indent=2))
    if payload.get("warn"):
        print(f"\n警告: {payload['warn']}", file=sys.stderr)
        return 1
    if payload.get("cpu", -1) < 0 or payload.get("mem", -1) < 0:
        print("\n错误: CPU/内存指标不可用。", file=sys.stderr)
        return 1
    return 0


def cmd_status(args) -> int:
    cfg = load_config(Path(args.config))
    agent = Agent(cfg)
    print(json.dumps(agent.status(), ensure_ascii=False, indent=2))
    return 0


def cmd_run(args) -> int:
    cfg = load_config(Path(args.config))
    if not cfg.get("token"):
        print("配置缺少 token，拒绝无鉴权运行。请先执行 install。", file=sys.stderr)
        return 1
    agent = Agent(cfg, verbose=args.verbose)
    agent.start()
    try:
        server = AgentHTTPServer((cfg["bind"], int(cfg["port"])), StatusRequestHandler, agent)
    except OSError as exc:
        print(f"监听 {cfg['bind']}:{cfg['port']} 失败: {exc}", file=sys.stderr)
        return 1
    print(f"AI Passport agent 监听 http://{cfg['bind']}:{cfg['port']}/status (panel={agent.panel})")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()
        agent.stop()
    return 0


def build_parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(
        description="AI Passport 服务器状态中转（1Panel / 宝塔 / 本机指标）")
    parser.add_argument("--version", action="version", version=VERSION)
    sub = parser.add_subparsers(dest="command", required=True)

    install = sub.add_parser("install", help="安装并启动 systemd 服务")
    install.add_argument("--bind", default="", help="监听地址，默认 127.0.0.1")
    install.add_argument("--port", type=int, default=0, help="监听端口，默认 8899")
    install.add_argument("--token", default="", help="访问 Token，默认自动生成")
    install.add_argument("--panel", default="", choices=["auto", "1panel", "baota", "local"],
                         help="面板类型，默认自动探测")
    install.add_argument("--panel-url", default="", help="面板地址，如 http://127.0.0.1:8090")
    install.add_argument("--api-key", default="", help="面板 API 密钥")
    install.add_argument("--ssl-host", default=None, help="要检查证书到期的域名（可选）")
    install.add_argument("--no-start", action="store_true", help="只安装不立即启动")
    install.set_defaults(func=cmd_install)

    uninstall = sub.add_parser("uninstall", help="卸载服务与文件")
    uninstall.add_argument("--keep-config", action="store_true", help="保留 /etc/ai-passport")
    uninstall.add_argument("--purge", action="store_true", help="同时删除趋势数据")
    uninstall.set_defaults(func=cmd_uninstall)

    test = sub.add_parser("test", help="立即拉取一次并打印 JSON")
    test.add_argument("--config", default=str(CONFIG_PATH))
    test.set_defaults(func=cmd_test)

    status = sub.add_parser("status", help="打印一次状态 JSON（不启动服务）")
    status.add_argument("--config", default=str(CONFIG_PATH))
    status.set_defaults(func=cmd_status)

    run = sub.add_parser("run", help="前台运行 HTTP 服务")
    run.add_argument("--config", default=str(CONFIG_PATH))
    run.add_argument("--verbose", action="store_true")
    run.set_defaults(func=cmd_run)
    return parser


def main(argv=None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
