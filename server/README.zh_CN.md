<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# AI Passport 服务器端中转

每台被监控的服务器上运行这一个单文件脚本。它替你调用 1Panel 或宝塔面板
的官方 API（完成签名），再对外输出一个极简 JSON 接口，设备只做一次带
Token 的 `GET`。

## 为什么需要中转

1Panel 和宝塔都要求用 API 密钥加时间戳签名。把签名逻辑留在服务器上，
ESP32 就不必保存面板密钥，不必把面板端口暴露到公网，也不必解析面板各自
不同的响应格式。

```text
ESP32 ── HTTPS ──> Caddy/Nginx ── HTTP ──> 127.0.0.1:8899 ──> 1Panel / 宝塔 API
                                                     └────> /proc、/sys 兜底
```

脚本还会每 `trend_step / 5` 秒采样一次本机指标，在内存中保留 24 小时趋势，
并持久化到 `/var/lib/ai-passport/trend.json`。设备熄屏期间趋势仍持续积累。

## 环境要求

- Linux，Python 3.6 或更高版本（CentOS 7 自带的 3.6.8 可直接运行）。
- 建议有 `systemd`；没有也能手动运行 HTTP 服务。
- 可选：已开启 API 的 1Panel 2.x 或宝塔 9.x。
- 不依赖任何第三方 Python 包。

## 安装

把 `ai_passport_agent.py` 传到服务器，用 root 运行：

```bash
sudo python3 ai_passport_agent.py install
```

不带参数时，脚本监听 `127.0.0.1:8899`，自动探测面板，仅使用 `/proc`
指标，并随机生成访问 Token。Token 只打印一次，保存在
`/etc/ai-passport/agent.json`，文件权限 `0600`。

### 1Panel

在面板界面启用 API，然后执行：

```bash
sudo python3 ai_passport_agent.py install \
    --panel 1panel \
    --api-key '<API 密钥>' \
    --port 8899
```

密钥来自面板的 API 设置页。脚本以 `1Panel-Token`、`1Panel-Timestamp`
请求头调用 `GET /api/v2/dashboard/current/all/all`。

### 宝塔

```bash
sudo python3 ai_passport_agent.py install \
    --panel baota \
    --api-key '<API 密钥>' \
    --ssl-host example.com
```

密钥来自**面板设置 → API 接口**。脚本向 `/system` 提交
`GetSystemTotal`、`GetDiskInfo`、`GetNetWorkApi` 三个动作。
`--ssl-host` 可选，仅用于启用证书到期天数。

面板不在本机回环地址时，追加 `--panel-url http://10.0.0.5:8090`。

### 纯本机模式

`--panel local` 完全跳过面板，只报告 `/proc` 指标。

## 图形化安装（1Panel / 宝塔）

不想敲命令时，用面板自带的**文件管理 + 计划任务 + 网站反向代理**就能装完，
不需要手写 nginx 配置。

| 步骤 | 1Panel | 宝塔 |
| --- | --- | --- |
| 1. 上传脚本 | 主机 → 文件 → 进入 `/opt` → 上传 `ai_passport_agent.py`（可从 Releases 下载） | 文件 → 进入 `/opt` → 上传 `ai_passport_agent.py` |
| 2. 计划任务安装 | 计划任务 → 创建 → 类型「Shell 脚本」→ 周期随意 → 粘贴下面 1Panel 的命令 → 保存后点「执行」 | 计划任务 → 添加 → 类型「Shell 脚本」→ 周期随意 → 粘贴宝塔的命令 → 保存后点「执行」 |
| 3. 复制 Token | 打开任务日志，找到「访问令牌」一行，复制保存；成功后暂停或删除该任务 | 同上 |
| 4. 验证 | 新建临时计划任务执行 `curl` 命令，在日志中查看返回 | 同上 |
| 5. 建站 | 网站 → 创建网站 → 类型选「静态」，域名填 `monitor.example.com` | 网站 → 添加站点 → PHP 版本选「纯静态」，域名填 `monitor.example.com` |
| 6. 反向代理 | 网站设置 → 反向代理 → 添加：代理地址 `http://127.0.0.1:8899` | 设置 → 反向代理 → 添加：目标 URL `http://127.0.0.1:8899`，发送域名 `$host`，缓存关闭 |
| 7. 证书 | 网站 → HTTPS → Let's Encrypt 申请 → 打开「强制 HTTPS」 | SSL → Let's Encrypt 申请 → 打开「强制 HTTPS」 |
| 8. 放行端口 | 主机 → 防火墙，放行 80、443 | 安全，放行 80、443 |

第 2 步的命令：

```bash
# 1Panel
python3 /opt/ai_passport_agent.py install --panel 1panel --api-key '<密钥>'

# 宝塔
python3 /opt/ai_passport_agent.py install --panel baota --api-key '<密钥>'
```

`--api-key` 是面板「面板设置 → API 接口」生成的密钥，不是自定义值。随便填
也能安装运行，但脚本会退回本机 `/proc` 指标，面板统计、Docker 和证书剩余
天数不可用；需要这些数据时请开启面板 API 并填入真实密钥。

安装脚本会创建并启动 systemd 服务 `ai-passport-agent`，随机生成访问
Token 并写入 `/etc/ai-passport/agent.json`（权限 `0600`）。日志里的
「访问令牌」只打印一次，请先复制保存。

面板里没有「计划任务」入口时，改用终端或 SSH 执行同样的命令；终端中需要
root，可在命令前加 `sudo`。

装完用浏览器打开 `https://monitor.example.com/status` 会看到
`{"ok":false,"error":"unauthorized"}`，这是**正常现象**：浏览器不会带
`X-Api-Token`。再建一个临时计划任务执行下面的验证命令，在任务日志里查看返回：

```bash
TOKEN=$(python3 -c 'import json;print(json.load(open("/etc/ai-passport/agent.json"))["token"])')
curl -s -H "X-Api-Token: $TOKEN" http://127.0.0.1:8899/status
```

设备配网页里填 `https://monitor.example.com/status` 和同一个 Token 即可。

图形化卸载：面板终端执行
`sudo python3 /opt/ai_passport_agent.py uninstall --purge`，再在网站列表里
删除那个站点。

## 用 HTTPS 对外发布

让脚本继续只监听回环地址，把 TLS 放在它前面。Caddyfile 示例
（另见 [Caddyfile.example](Caddyfile.example)）：

```caddyfile
monitor.example.com {
    reverse_proxy 127.0.0.1:8899
}
```

然后在设备配网页填写 `https://monitor.example.com/status`。设备使用
ESP-IDF 内置 CA 证书库校验证书，因此必须是公共 CA 签发的证书；自签名
证书会被设备拒绝。

## 接口

`GET /status`，请求头 `X-Api-Token: <token>`，返回：

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

| 字段 | 含义 |
| --- | --- |
| `cpu`、`mem`、`swap`、`disk` | 百分比，0-100。面板未提供时为 `-1`。 |
| `load1` | 1 分钟平均负载。 |
| `net_rx`、`net_tx` | 采样窗口内的每秒字节数。 |
| `disk_read`、`disk_write` | 采样窗口内整盘读/写每秒字节数。 |
| `uptime` | 开机秒数。 |
| `temp` | 摄氏度，存在温度传感器或面板值时提供。 |
| `docker_run`、`docker_total`、`docker_bad` | 运行中、总数、异常（非 running）容器数，有 Docker 时提供。 |
| `ssl_days` | `ssl_host` 证书剩余天数；未配置或不可达时为 `-1`。 |
| `trend.step` | 每个趋势点的间隔秒数。 |
| `trend.cpu`、`trend.mem` | 最多 288 个采样，按时间从旧到新。 |
| `warn` | 仅当面板调用失败、改用本机指标时出现。 |

`GET /healthz` 不需要鉴权，返回 `{"ok": true, "version": "..."}`，可用于
存活探测。Token 缺失或错误时返回 `401`。

## 验证

```bash
sudo python3 /opt/ai-passport/ai_passport_agent.py test
curl -H "X-Api-Token: $(sudo python3 -c 'import json;print(json.load(open("/etc/ai-passport/agent.json"))["token"])')" \
     http://127.0.0.1:8899/status
```

面板调用失败或 CPU/内存不可用时，`test` 以非零状态退出。

## 日常操作

```bash
sudo systemctl status ai-passport-agent
sudo journalctl -u ai-passport-agent -n 50
```

修改配置：编辑 `/etc/ai-passport/agent.json`，然后
`sudo systemctl restart ai-passport-agent`。要换面板密钥，用相同参数重新
执行 `install` 即可，原有 Token 和趋势数据会保留。

## 卸载

```bash
sudo python3 ai_passport_agent.py uninstall             # 保留 /etc/ai-passport
sudo python3 ai_passport_agent.py uninstall --purge     # 同时删除趋势数据
```

卸载会停止并禁用服务，删除 unit 文件和 `/opt/ai-passport`；除非指定
`--keep-config`，否则也会删除 `/etc/ai-passport`。

## 常见问题

- **`401 unauthorized`**：Token 不匹配。先核对设备配网页里的 Token，再核对
  `agent.json` 中的值。
- **`panel` 显示本机兜底且带 `warn`**：面板密钥或地址有误，或面板 API 未
  开启。执行 `test` 查看具体 HTTP 错误。
- **`cpu` 或 `mem` 为 `-1`**：面板响应缺少该字段，脚本改用 `/proc`；启动后
  需要等待一个采样周期。
- **设备提示对时失败**：TLS 需要正确的系统时间。放行出站 UDP 123，或改用
  可达的 NTP 服务器。
- **设备提示证书错误**：请使用公共 CA 证书。浏览器可能接受自签名证书，但
  设备会拒绝。

## 安全

- 除非有可信内网，否则保持监听 `127.0.0.1`。
- Token 是 `/status` 的唯一访问控制，避免出现在截图和日志里。修改
  `agent.json` 后重启服务即可轮换。
- 面板密钥只保存在 `agent.json`（权限 `0600`），不会下发到设备。
- 脚本默认通过 HTTP 访问同机面板，并跳过这些本机调用的证书校验；不要把
  `--panel-url` 指向不受信任的远程主机。
