[English](webui.md)

# Web UI

Radxa Linkr Debugger 固件 HTTP 和 WebSocket API 的 React/Vite 控制界面。
生产固件会将 gzip 压缩的 UI 嵌入 flash，通过 USB NCM 在
`http://172.29.203.1/` 提供服务。页面使用同源 `/api/v1` HTTP 和
WebSocket 端点，正常板级控制不需要主机代理。

## 访问

连接 USB NCM 接口后打开：

```text
http://172.29.203.1/
```

## 强制门户发现

固件实现了多路径强制门户检测辅助功能，在 host 接入 NCM 链路时最大化
操作系统自动打开调试板 Web UI 的概率。

- **DHCP**：NCM 接口上的 DHCPv4 server 将路由器和 DNS 地址通告为
  `172.29.203.1`，并发送 DHCP option 114（Captive Portal URI），值为
  `http://172.29.203.1/captive-portal/api`。这是面向兼容性的 HTTP 端点，
  不是可信的 HTTPS 信号。
- **DNS**：NCM 接口上的轻量级 DNS 响应器绑定 UDP port 53，对任何传入查询
  均返回指向 `172.29.203.1` 的泛解析 A 记录。AAAA 查询返回 NOERROR 但
  answers 为零（NODATA）。DNS TTL 为 30 秒。
- **HTTP port 80**：绑定在 `172.29.203.1:80` 的单一 Zephyr HTTP 服务按
  URL 路径路由。GET 请求到达 `/captive-portal/api` 时返回 HTTP 200 和
  JSON 正文：
  `{"captive":true,"user-portal-url":"http://172.29.203.1/","venue-info-url":"http://172.29.203.1/"}`。
  其他不匹配路由的 GET 路径返回 HTTP 302，`Location` 指向
  `http://172.29.203.1/`。
- **自动打开为 best-effort，不保证。** 结果因操作系统版本、网络配置和
  主机是否有其他活动网络接口而异。需要 Web UI 时，用户始终可以在浏览器中
  直接打开 `http://172.29.203.1/`，或使用 `curl`、CLI/TUI。

## 仪表盘概览

主仪表盘提供以下卡片：

- **电源控制** — 开关 `12v_out`、`5v_out`、`20v_out`。
- **ADC 监测** — 各电源轨的实时电流读数。
- **Switch 路由** — TF/SD 在 `target` 和 `usb-reader` 之间切换；J12 下层插入的
  USB 设备可在主机（`pc`）和 J12 上层所接目标板（`target`）之间切换。
- **GPIO** — 读写安全 GPIO（`GP7`-`GP20`、`GP29`）。

电源与电流卡片包含触发式功率分析仪。支持手动、电流阈值、GPIO 边沿和
上电触发，保留四次捕获用于叠加对比，可导出 CSV 或 NDJSON。捕获使用
固件设备时间戳和预/后触发环形缓冲区。

## Terminal 工作区

Terminal 工作区与主仪表盘并列，包含：

- **逻辑分析仪** — RP2350 PIO2+DMA 高速单次捕获。支持安全引脚列表中的
  1-16 个通道（GP7-GP9、GP10-GP20、GP29），触发模式 `none`、`rising`、
  `falling`、`either`，请求采样率 1-125 MHz，最多导出 512 个样本。
  完成的捕获可在波形视图中预览，并导出为 CSV 或 PulseView `.sr` 文件。
  还提供连续流式模式（1-25 MHz）。浏览器内 Rust/WASM 解码器支持
  UART、I2C 和 SPI 协议解码。
- **串口终端** — 通过 CH347F 提供独立的 UART0 和 UART1 会话。Tab 模式
  切换可见终端而不中断任一通道；分屏模式同时显示两个终端。使用 xterm
  兼容终端界面：直接点击并输入。默认发送 CRLF；工具栏可切换 CR 和
  LF 模式。

## 高级与恢复

- **OTA 卡片** — 通过同一 USB NCM HTTP API 交付 RP2350 固件更新。
  完整 OTA 工作流参见 [OTA 固件更新](ota.zh-CN.md)。
- **启动功率分析** — 见下方专节。

## 串口控制台

### Web Serial（安全来源）

Edge 和 Chrome/Chromium 可以从安全来源（如本地 Vite 开发服务器或 GitHub
Pages）直接使用 Web Serial。用户需要点击 **Web Serial** 按钮并接受
浏览器设备选择器。

### 板载页面

板载页面运行在 `http://172.29.203.1/`，使用明文 HTTP，默认不是安全上下文。
要从板载页面启用直接 CH347 Web Serial：

1. 复制下方 Chromium 标志页地址，手动粘贴到地址栏（普通网页无法导航到
   `chrome://` URL）：

   ```text
   chrome://flags/#unsafely-treat-insecure-origin-as-secure
   ```

2. 添加精确来源：

   ```text
   http://172.29.203.1
   ```

3. 启用标志并重启浏览器。

Edge 也接受此 Chromium 地址。

### 设备桥接 fallback

如果不启用上述标志，保持板载页面打开，改用主机侧桥接：

```sh
cd web
npm ci
npm run device-bridge
```

然后在任一串口终端中使用 **Bridge** 按钮。桥接优先使用 CH347F `D1`
设备对应 UART0、`D3` 对应 UART1，当这些后缀不可用时回退到排序设备顺序。

### Linux 串口设备权限

直连 Web Serial 和设备桥接都会以当前桌面用户身份打开串口设备。在 Linux
上，CH347 串口通常显示为 `/dev/ttyUSB0` 和 `/dev/ttyUSB1`；板载 CDC ACM
fallback 可能显示为 `/dev/ttyACM0`。

如果浏览器提示无法打开串口，或 Bridge 报告 `EACCES`、`EPERM`、
`Permission denied`、`Access denied`，先检查设备所有者和当前用户组：

```sh
ls -l /dev/ttyUSB* /dev/ttyACM* 2>/dev/null
id -nG
```

Debian 和 Ubuntu 通常把串口设备分配给 `dialout` 组。执行：

```sh
sudo usermod -aG dialout "$(id -un)"
```

随后注销并重新登录，或直接重启系统；重新插拔调试板，重启浏览器或
`npm run device-bridge`，再尝试连接。已经运行的浏览器和终端进程不会自动
获得新增的用户组权限。

如果设备属于其他串口访问组，请使用 `ls -l` 显示的组，并遵循当前发行版
的设备权限规则。如果权限已经正确，再检查是否有串口监视器或系统服务占用：

```sh
fuser /dev/ttyACM0
```

请把路径替换成实际打开失败的设备。不要用 root 身份运行浏览器或 Bridge，
也不要把 `chmod` 当作永久修复：USB 设备重新连接后会重新创建设备权限。

## 启动功率分析

启动工作流位于 **高级与恢复** 下。需要已选择的 UART0 或 UART1 连接
以及空闲的功率捕获会话。经一次明确确认后：

1. 清除并录制目标串口控制台。
2. 关闭所选电源轨并等待配置的放电延迟。
3. 在恢复供电前 arm 固件 `power_on` 捕获。
4. 从返回的串口数据中标记首个上电后 UART 字节、U-Boot 或 UEFI、内核
   和登录标记。
5. 报告峰值电流、平均功率、积分能量和最近两条触发对齐的功率曲线。

启动固件选择器默认自动检测，可固定为 U-Boot 或 UEFI。任务可在上电前
取消。完成后可下载串口日志。

## GitHub Pages 部署

推送到 `dev` 分支会将生产构建部署到
<https://xzl01.github.io/agent-debugboard/>。GitHub Pages 通过 HTTPS 提供
UI，而板载 API 通过 USB-NCM 网络上的 HTTP 暴露。从托管页面使用硬件控制
前需要启动设备桥接网关：

```sh
cd web
npm ci
npm run device-bridge
```

Pages 构建连接到 `http://127.0.0.1:8787/api/v1`。网关默认将 HTTP 和
WebSocket 流量直接转发到固件服务 `http://172.29.203.1:8080`，并提供
浏览器所需的 CORS 和 Private Network Access 头。需要时可覆盖上游地址：

```sh
LINKR_BOARD_URL=http://172.29.203.1:8080 npm run device-bridge
```

## 相关文档

- [OTA 固件更新](ota.zh-CN.md)
- [OpenOCD / JTAG](openocd.zh-CN.md)
