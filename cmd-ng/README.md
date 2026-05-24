# cmd-ng

`cmd-ng/` 是当前主力开发的 Rust 主机侧 CLI/TUI 实现。

当前目标是把主机侧 CLI/TUI 的主线能力收敛到 Rust 实现，并保留“无参数进入 TUI、有参数进入传统 CLI”的入口契约。仓库里的旧 Go `cmd/agent-debugboardctl` 路径进入 deprecated/legacy 维护状态，不再作为主力开发方向。

## 当前范围

- CLI：`status`、`doctor`、`power list|get|set`、`adc read`、`adc record`、`sd get|route`、`gpio list|set|input`、`watchdog status`、`bootloader`
- 输出：支持 `--json`，并校验 `agent-debugboard.v1` envelope
- TUI：无参数启动，是当前主力交互入口，并持续向现有 Go 行为收敛

## 构建与运行

```sh
cargo run --manifest-path cmd-ng/Cargo.toml -- --help
cargo run --manifest-path cmd-ng/Cargo.toml -- --json status
cargo run --manifest-path cmd-ng/Cargo.toml --
```

## 说明

- 默认设备 URL 仍是 `http://172.29.203.1:8080`
- `--json` 仍要求固件返回 `schema/ok/command`
- TUI 现在以 HTTP 轮询作为主数据通道，因此可以稳定多开；实时高频采集改由 `adc record` 走 websocket
- TUI 控件区把 power 和 GPIO 合并进同一个自适应网格，方向键/Tab 统一导航，Space/Enter 切换当前项，`i` 把当前 GPIO 切回输入
- GPIO 在 CLI/TUI 中会同时显示 `GPxx` 和 `note`；控制时可使用 `GPxx`、数字引脚（如 `4`）或精确 note（如 `CON_MAS`）
- `adc record OUTPUT_PATH [MAX_SAMPLES]` 会创建 live websocket session，以 1000Hz 订阅 telemetry 并写成 NDJSON 文件
- FIXME: `adc record` 目前固定按 1000Hz 订阅，后续应增加可配置采样/订阅速率参数，并同时区分设备侧采样节拍与主机侧接收时间戳
- `raw` 模式与 Go 版一致：HTTP 路径下不支持
- `watchdog` 仍只暴露 `status`，不提供 host 侧 feed/控制
- 旧 Go `cmd/agent-debugboardctl` 路径仅保留作 legacy 参考与回归对照
