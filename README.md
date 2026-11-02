**简体中文** · [English](README.en.md)

<h1 align="center">Server-M 监控</h1>

<p align="center">
  <strong>把 FoloToy AI Passport 变成一台桌面服务器状态屏</strong><br>
  ESP32-C3 · 240 × 320 · 三键操作 · 网页配网
</p>

<p align="center">
  <a href="https://github.com/trace-gu/Server-M/releases/latest"><img src="https://img.shields.io/github/v/release/trace-gu/Server-M?style=flat-square" alt="Latest release"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-MIT-64748b?style=flat-square" alt="MIT License"></a>
</p>

Server-M 是运行在 FoloToy AI Passport 硬件上的服务器监控固件。它通过中转地址轮询服务器状态，在 240 × 320 屏幕上显示 CPU、内存、磁盘、网络、温度和容器等指标，并支持 24 小时趋势、熄屏省电与多网络切换。

## 功能

- 最多保存 20 台服务器，列表显示在线状态和 CPU、内存概览。
- 详情页显示 CPU、内存、Swap、磁盘、负载、网络吞吐、磁盘 I/O、运行时间、温度、TLS 证书剩余天数、Docker 容器状态和备份状态。
- 每台服务器在设备上保留 24 小时趋势。
- 刷新间隔 1–300 秒可调，亮度 10–100%，熄屏时间可设为常亮。
- 最多保存 6 个 Wi-Fi 网络；断线自动重试，连续失败后切换到下一个网络。
- 手机网页配网，无需安装 App，也不使用蓝牙。

## 硬件要求

| 项目 | 要求 |
| --- | --- |
| 设备 | FoloToy AI Passport（ESP32-C3，8 MB Flash，无 PSRAM） |
| 屏幕 | ST7789P3，240 × 320 |
| 按键 | UP / DOWN / OK |
| 网络 | 2.4 GHz Wi-Fi，以及一台运行中转服务、可被设备访问的主机 |

## 烧录预编译固件

1. 从 [Releases](https://github.com/trace-gu/Server-M/releases/latest) 下载 `Server-M-full.bin`。
2. 用数据线把设备连接到电脑，安装 `esptool`：

```bash
python -m pip install esptool
```

3. 写入从 `0x0` 开始的合并镜像：

```bash
python -m esptool --chip esp32c3 -p <串口> -b 460800 write_flash 0x0 Server-M-full.bin
```

`<串口>` 在 Windows 上是 `COM3` 之类的名称，在 macOS/Linux 上是 `/dev/tty.usbmodem*` 或 `/dev/ttyACM*`。

> [!WARNING]
> 从 `0x0` 写入合并镜像会覆盖应用和 NVS 数据，已保存的服务器、Wi-Fi 和显示设置会被清除。本固件没有启用安全启动或 Flash 加密，不需要先备份原固件。

## 首次配网

1. 设备首次开机且没有保存网络时自动进入配网模式，屏幕显示热点名 `AI-Passport-XXXX` 和本次开机随机生成的 8 位密码。
2. 手机连接该热点，浏览器打开 `http://192.168.4.1`。
3. 按页面顺序填写 **服务器 → 显示设置 → Wi-Fi**。
4. 点击「连接并退出配网」。设备保存配置、关闭热点并直接连接该 Wi-Fi。
5. 配网结束后，在设备「设置 → Wi-Fi 网络」中查看新 IP，也可以在路由器的设备列表里查找主机名 `ai-passport`。

配网流程、HTTP 接口和验收步骤见[网页配网文档](docs/development/engineering/wifi-provisioning.zh_CN.md)。

## 从源码构建

需要 ESP-IDF 5.5.3 和 Python 3。

```bash
git clone https://github.com/trace-gu/Server-M.git
cd Server-M
. $IDF_PATH/export.sh
idf.py set-target esp32c3
idf.py build
```

生成并校验可烧录的合并镜像：

```bash
./tools/validate.sh --firmware
```

产物位于 `build/Server-M-full.bin`，烧录偏移为 `0x0`。只运行静态检查和主机测试：

```bash
./tools/validate.sh --static
```

环境准备见[环境搭建](docs/development/engineering/environment-setup.zh_CN.md)，完整门禁见[构建与测试](docs/development/engineering/build-and-test.zh_CN.md)。

## 按键

| 页面 | UP | DOWN | OK | 长按 OK |
| --- | --- | --- | --- | --- |
| 服务器列表 | 上一台 | 下一台 | 进入详情 | 打开设置 |
| 服务器详情 | 上一台 | 下一台 | 立即刷新 | 返回列表 |
| 设置 | 上移 | 下移 | 修改当前项 | 返回列表 |
| Wi-Fi 网络 | 上移 | 下移 | 切换网络或开配网 | 返回设置 |

熄屏状态下按任意键先亮屏，再恢复 Wi-Fi 连接。

## 目录结构

```text
components/bsp/  板级支持包：显示、按键、音频、电池、共享 I2C
main/            Server-M 应用：界面、配置、Wi-Fi 状态机、HTTP 客户端和配网页
tests/           可在主机运行的纯逻辑测试
tools/           校验、固件合并与归档脚本
docs/            开发、硬件、贡献指南和社区参考资料
```

## 文档

| 文档 | 内容 |
| --- | --- |
| [项目概览](docs/README.zh_CN.md) | 仓库定位、能力清单和文档索引 |
| [网页配网](docs/development/engineering/wifi-provisioning.zh_CN.md) | 配网流程、接口和验收 |
| [硬件开发指南](docs/hardware-design/AI_HARDWARE_DEVELOPMENT_GUIDE.zh_CN.md) | 引脚、总线、显示与音频约束 |
| [构建与测试](docs/development/engineering/build-and-test.zh_CN.md) | 门禁、主机测试与固件校验 |
| [AI 开发入口](AGENTS.zh_CN.md) | AI 代理必须读取的任务路由 |
| [开发记录](docs/CHANGELOG.zh_CN.md) | 版本变更历史 |

## 许可证

[MIT](LICENSE)
