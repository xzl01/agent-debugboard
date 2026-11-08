# 2026-11-06–07：WebUSB 地址引导实现与验收边界

实现和离线验证始于11月6日，最终 `usb-only` 实机续验于11月7日完成。

## 状态

**离线、构建、浏览器页面和 `usb-only` 受限板级 HIL 通过。** 用户明确移除 PD 供电并授权
排除相关项目后，已同步 AGENTS 与 canonical HIL 规范并加载新规则；PD20、电源和相关
ADC/INA 项不属于最终受限验收，不声明完整 HIL 通过。
Pages 未发布，没有提交或 push。不能据此声称实际连接通知或线上引导链路已经可用。

## 实现

- 固件复用现有持久化 network identity，在 USB 初始化时预生成
  `https://xzl01.github.io/agent-debugboard/connect.html#device=<board IPv4>`。
- 仅增加 BOS 能力和设备级 GET_URL，不新增 USB 数据接口/端点，不改变 NCM/CDC 或 HTTP/WS 协议。
- `bcdUSB=0x0210`，而非仅满足 Zephyr BOS 门槛的 `0x0201`；已核对 Chromium Linux
  `usb_service_linux.cc:39-46` 的自动探测门槛。USB2 Extension 属性为0，不宣称 LPM。
- URL 是启动快照；请求回调只复制已准备字节，不读写 NVS。身份准备失败时不注册 WebUSB，原 USB 初始化继续。
- Pages 专用 `connect.html` / `connect.js` 不挂载 dashboard，只接受单一规范 RFC1918 IPv4
  fragment，显示地址并提供用户主动 HTTP 顶层导航。无自动跳转、扫描、设备 API 请求或浏览器持久化。
- 完整协议见 [WebUSB handoff](../../reference/webusb-landing.md)，使用方式见
  [Web UI](../../user/webui.zh-CN.md#webusb-主页提示)。

## 有效验证

| 命令／场景 | 结果 |
| --- | --- |
| `make gates unit-tests` | exit0，383门禁及全部注册 C/Python 测试通过，含新 URL encoder |
| `cd web && npm test` | exit0，Node457通过/1个既有 opt-in loopback skip，Vitest1089通过 |
| Pages 环境 `npm run build`，项目 Nix shell | exit0，引导两文件与 public 逐字节一致 |
| `make firmware` | exit0，完整 sysbuild 和 combined UF2 生成 |
| Chromium156 真实渲染与交互 | 37组断言通过，1440px/375px、missing/invalid/ready、无JS、键盘激活 |

宿主环境首次 Pages 构建缺 `wasm-bindgen`，未修改源码规避；在项目 Nix shell 重跑通过。
浏览器通过 Fetch 拦截使用本地页面/脚本，并非线上部署。点击前设备请求0；显式激活后的
唯一私网 HTTP 导航由固定 fixture 返回，没有访问实际板卡。Console/Runtime/CSP 错误均0。
observer 独立复核桌面和窄屏截图，无截断、重叠或“已连接/已验证设备”误导。

新固件 Flash **825916/843736 B**，RAM **485296/532480 B**，比本轮前增加800B/80B，
8KiB Flash/32KiB RAM 余量门禁不变。引导文件未进入板载 Web 资源集；WASM hash 仍为
`3f42e618681520ac5e187769d538b1e5b5d90f90d80922890a6205fed5f74c54`。
新 combined UF2 SHA-256：
`887201e6fee4c00a87c0678c30c03f2f30310566f2783fd6520a4a12f88d1571`。

## 板级阻塞与旧镜像对照

仅操作用户授权 USB `1-7.4.2`、serial `F2836E780B95F053`。
新镜像 load/verify 和 NCM 启动成功；首次 host hardware mutation 为 `5v_out=on`，
随后 PD20 设置返回 HTTP500 `set_failed`，供电预检未通过。

只读状态曾为 PD `available=false`、`last_error=-67`、5Vout实测0V；用户恢复供电后，
一度看到约5.04V输出和约5.10V输入，但仍无 PD 协议或 PDO，随后输入约1.07V、输出0V。
用户明确允许的新一轮预检也未通过；后续有界只读观察仍未见稳定 PD，未再次重放设置。

用户授权回刷上一版已验证的
`7b7b5815c1378934d2cfbe251309d1b756d4a325b5e9dd08194da801baef9809`
作诊断对照。load/verify成功，运行时 `bcdUSB=0x0200`、RAM485216B确认旧镜像启动；
其开机 PD20 预检同样失败，输入约1.07V、5Vout为0V。该观察不支持“仅新增 WebUSB 才出现”
的归因，但没有替代外部供电检查，也不能据此宣布新固件硬件验收通过。

用户随后重新插拔上电并报告 VBUS20V。同序列号板卡最新状态为 PD 协议有效、包含20V PDO、
ADC0约19.25V；此时5Vout是新上电默认off，输出0V不能解释为VBUS缺失。旧版新一轮预检
通过（INA5.25V）后，已重新刷入本轮新镜像，新版预检也通过（INA5.111V）。当前板卡为
新版 `887201e6…`，没有绕过5Vout/PD20/有效INA门槛，也没有据早先读数认定用户供电故障。

原始 USB 节点为 root:root 0644，libusb open返回 EACCES，未能发送 BOS/GET_URL 请求。
用户允许本轮临时单节点 ACL；供电恢复后尝试 `sudo -n setfacl`，因需要本地密码而失败，
ACL没有应用，未修改 udev/系统配置。可由用户在终端一次性 sudo 执行同一只读描述符脚本，
不必授予持久访问权限；脚本先核验指定序列号和有效PD20/INA状态，不 claim/detach接口。
首次组合命令缺少 fail-fast，预检失败后仍尝试了一次只读 USB open；在权限检查即失败，
没有实际控制传输。后续步骤已改为先确保预检成功再运行。

诊断 CDC 首次读日志时未设 raw，TTY ECHO 曾回送日志形成 unknown-command 回显；
已关闭 ECHO、恢复 raw 并关闭本轮 fd。没有将该诊断伪象当成固件问题。

## 最终 `usb-only` 受限验收

用户说明 PD 供电已经移除，明确授权忽略相关项目。默认 `full` 规则及其历史预检结果保留；
新增的 `usb-only` 不请求 PD，也不以缺少外供电的 INA 数值阻塞 USB 控制面。每次启动后的
首条 host hardware mutation 仍明确设置 `5v_out=on` 并回读逻辑状态，排除项记未测而非通过。

用户在本机用 sudo 执行只读描述符脚本 `--profile usb-only`，没有修改 ACL/udev，也没有
claim/detach接口。主模型已读取原始返回字节及结果文件，不仅依据用户终端摘要：

| 场景 | 实机结果 |
| --- | --- |
| Device descriptor | `bcdUSB=0x0210` |
| BOS | 36B，两 capability：零属性 USB2 Extension、WebUSB UUID/version/vendor/index 正确 |
| GET_URL | 68B，返回 `https://xzl01.github.io/agent-debugboard/connect.html#device=10.42.51.125` |
| 短 `wLength` | 1、3、67B均为完整URL descriptor的正确前缀 |
| 非法索引 | `wValue=0/2/0x101`、`wIndex=1/0x102`均STALL，随后合法请求正常 |
| USB 配置兼容 | configuration/interface/endpoint descriptor 字节与旧基线完全相同 |
| URL交给实际页面parser | 得到同一板卡的 `http://10.42.51.125/`，无任意URL或额外路径 |
| NCM HTTP/WS | status与WS status snapshot正常，watchdog healthy、J4 ready |
| 正常reboot | 启动和usb-only逻辑fixture通过 |
| HTTP与控制CDC BOOTSEL | 均进入指定serial的ROM，combined UF2恢复和启动通过 |

最终仍仅为 USB `1-7.4.2`、serial `F2836E780B95F053` 上的新镜像 `887201e6…`。
PD20、外部电源轨/负载、ADC/INA精度、功率采集、外部目标/GPIO驱动、采集吞吐不在本次
最终profile内；未调用这些功能来凑完整HIL。先前供电预检失败没有被改记为PASS。

实际浏览器通知受平台策略和普通用户USB权限约束，root手工读取不等于正常Chrome自动提示。
浏览器UI验收使用HTTPS origin本地资源拦截，不是线上部署。匹配 Pages 版本仍需单独发布，
本轮没有远端写授权。

证据：`/tmp/opencode/webusb-impl-2026-11-06/`，包括 `validation/`、`design/`、
`final/image.json`、`hil/usb-descriptors.json`、`wire-to-page.json`、`control-smoke.json`、
`lifecycle.json` 和此前 `old-image-control.json` 等。fixture、受限验收、未测项和线上状态分别记录。
