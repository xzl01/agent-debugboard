# 2026-11-05：Web／WASM 等价瘦身与构建对照

## 边界与结论范围

本轮按用户要求做隔离构建、模拟数据功能对照与本地 Node／Chromium 计时。
用户明确禁止非授权 HIL：没有设备 API、USB／串口、实际采集、刷写、重启或电源操作。
本地静态 fixture 不转发真实设备请求，性能结果不是 MCU、USB 或实机验收。
基线是含此前 ADC／logic 内存和 codec7 改动的工作树，目标固件 Flash 为 835488 B，
RAM 为 482440 B。原有 Tray/Cargo/Nix/.ignore 暂存内容继续保留，本轮不提交或 push。

当前选择为 WASM 方案 D，保留原 Oxc Web 构建。选择依据只覆盖本文模拟输入及宿主
暖态解码；**未测页面冷启动、WASM 初始化／首调，也不保证所有浏览器或稳定尾延迟。**

## 隔离方法与可复现基线

实验源码、依赖副本、Cargo cache/target、临时配置及输出均放在
`/tmp/opencode/web-wasm-ab-2026-11-05/`。A/B 阶段主工作树与共享固件目录保持不变。
所有资源使用原构建的 Zopfli 1.0.3 `--i15`，验证标准 gzip、mtime=0、无 optional
header、原文精确解压和重复 hash；不把提高压缩迭代或删 metadata 算成本轮收益。

默认 Rust 控制构建逐字节复现原始 WASM、glue 及 gzip。私有 Cargo cache 最初改变了
依赖诊断路径；统一 remap 回原路径后才接受控制数据，而不是把路径变短算作优化。
使用 canonical Nix Rust/Cargo 1.95.0、LLVM 21.1.8、wasm-bindgen 0.2.121，而非宿主
rustup 的不同版本。WASM glue、公开接口、初始 17 页内存及 producers/target_features
metadata 保持一致，没有删解码器、语言、页面、错误文本或诊断。

## Web：拒绝保守 Terser 候选

固定浏览器 target、CSS、模块副作用、内联方式和资源请求布局，仅比较当前 Oxc 与
Terser 5.48.0 的保守配置。保留函数/类名、console、debugger、参数和 getter 副作用，
不使用 property mangle 或 unsafe 模式。五件 Vite+decoder 资源之外，CMake 的三项静态
资源保持不变；hidden sourcemap 只做临时归因，不进入产品。

| 方案 | app.js raw | app.js gzip | 五件 gzip 总量 | 比基线增加 |
| --- | ---: | ---: | ---: | ---: |
| 当前 Oxc | 1355100 B | 361923 B | 457260 B | — |
| Terser passes=1 | 1416388 B | 374198 B | 469535 B | 12275 B |
| Terser passes=2 | 1414922 B | 373581 B | 468918 B | 11658 B |

控制构建 JS/CSS/HTML 与原始资源 hash 完全一致。三组固定文件／诊断检查、101 项相关
Node 测试及最终本地 Chromium smoke 通过，但首次 smoke 有一项未归因的瞬态文本长度
差异；重跑通过不作为严格等价证明。由于两候选已在体积上落败，不扩大其运行性能验证，
不改变主项目 minifier 或依赖。

## WASM：真实配置与体积

| 方案 | 实际优化 | WASM raw | WASM gzip | 含glue gzip | 比基线少 |
| --- | --- | ---: | ---: | ---: | ---: |
| 原始／控制 | opt3、CGU16，无跨crate LTO | 227073 B | 81824 B | 83679 B | — |
| A | opt3、CGU1，无跨crate LTO | 212120 B | 78153 B | 80008 B | 3671 B |
| B | opt-s、CGU1，无跨crate LTO | 193845 B | 65972 B | 67827 B | 15852 B |
| C | opt-z、CGU1，无跨crate LTO | 185128 B | 64608 B | 66463 B | 17216 B |
| D | opt3、CGU1、单cdylib真实fat LTO | 187853 B | 68677 B | 70532 B | 13147 B |

A/B/C 虽请求 fat LTO，但 manifest 的 `cdylib+rlib` 组合使 Cargo 没有执行跨crate
LTO；依据实际 rustc 命令分类，不能把它们标成真实 fat-LTO 收益。D 在 Cargo 参数区
使用 `--lib --crate-type cdylib`，实际 rustc 仅有 cdylib、`-C lto=fat`、opt3、CGU1；
12 个依赖对象验证为 LLVM bitcode，最终链接通过。

所有 glue 字节完全相同。D 模块 SHA-256：
`3f42e618681520ac5e187769d538b1e5b5d90f90d80922890a6205fed5f74c54`。
D gzip SHA-256：`90945d000e84e794eb399f58435daca6104aa90c784f33ab91d13ed3e86cb7b5`。
13147 B 是 **decoder 模块加glue资源的15.7%**，不是整个 Web 或整镜像同比缩减。

每组现有 21 项 Rust 测试通过；original/control/A/B/C/D 的 44 组实际 WASM 完整输出
逐字节一致，覆盖 UART/I2C/SPI、协议参数、诊断、错误、Unicode 和边界输入。公开
imports/exports、memory/table ABI、malloc/realloc/free、memory.grow 与 glue 视图刷新
均检查通过；内部非导出函数表布局不要求相同。

## 暖态运行对照

构建全部结束后才串行计时，不并发 Node、浏览器或构建。Node 两轮预热后交错配对，
轮转四组长模拟输入并平衡先后顺序，保留完整样本。B 的成对中位耗时增加约31–42%，
C 增加约92–100%，因此拒绝，不能只选体积最小的 C。D 两轮成对中位比约0.82–0.85。
第二轮宿主尾部明显抖动，不能据绝对 p95 承诺延迟。

Chromium 156.0.8078.79／V8 15.6.75.27 的最终合格轮为每负载25轮、固定3次调用平均；
先后顺序各12/13次。计时包括真实 glue、WASM JSON parse／解码／诊断／序列化／复制，
排除加载、编译、初始化、fixture 生成及结果核对。

| 模拟负载 | 原始median | D median | 成对D/原始median |
| --- | ---: | ---: | ---: |
| UART 2048帧 | 5.1667 ms | 4.5333 ms | 0.8718 |
| I2C 1024字节 | 1.7667 ms | 1.5000 ms | 0.8654 |
| SPI 2048双工字 | 3.1667 ms | 2.7333 ms | 0.8557 |
| UART校验／停止位诊断 | 3.4000 ms | 2.9333 ms | 0.8600 |

即本机给定负载中，成对耗时中位数减少约12.8–14.4%；成对比值中位数并不等于两列
中位数相除。首个浏览器计时轮存在先后顺序偏置，已完整保留并排除，只使用修正且
核对顺序平衡后的最终一轮。十次页面请求全部为 loopback 静态白名单资源；浏览器正常
退出、端口与临时 profile 清理，未连接真实目标或运行产品设备控制。

## 最小落地与正式验收

采用专用 `wasm-release` profile，继承 release、显式 opt3/fat/CGU1，仅用于 WASM；
保留原 manifest 双 crate 类型和 native release 默认。同步本地脚本、CI 和 Nix 的
单cdylib构建及 profile 路径，不新增框架、Node依赖到Nix、运行时解压、请求或 heap。

正式 Web production build 与完整 `make firmware` 均重新生成 WASM，模块／glue hash
与实验 D **逐字节一致**，没有复制实验产物替代正式生成。实际 rustc 命令确认单
cdylib、opt3、`-C lto=fat`、CGU1；其余七份内嵌资源的 raw/gzip hash 与原始完全相同。

| 目标固件资源 | 基线 | 正式优化后 |
| --- | ---: | ---: |
| 全部内嵌资源 gzip | 458646 B | 445499 B |
| Flash image | 835488 B | **822340 B** |
| Flash 净减少 | — | **13148 B** |
| 847832 B 容量中的剩余量 | 12344 B | **25492 B** |
| 扣8 KiB硬门限后的增长余量 | 4152 B | **17300 B** |
| RAM image | 482440 B | **482440 B，不变** |

资源字节减少13147 B，整镜像实际减少13148 B，两者的一字节差来自链接布局／对齐，
不将压缩资源差值直接冒充整镜像差值。固定slot容量和资源门禁未调整。

正式离线检查均 exit 0：

- decoder `cargo fmt`、`cargo clippy --locked --all-targets --all-features`、
  `cargo test --locked --all-targets`：21项测试通过。
- 完整 Web：Node 457项通过、原有一项条件跳过；85个Vitest文件、1078项通过。
- Web production build、333项仓库门禁及三个checker、完整 `make firmware` 通过。
- `nix flake check -L --no-update-lock-file`：当前 **x86_64-linux** 全部检查通过；
  其他默认系统未构建，不外推跨平台验收。decoder的Nix产物确认使用`wasm-release`。

正式 combined UF2 SHA-256：
`53a8228c295b6ddb18d38cead9c17d10ce256957ecb7f1089d3e37b8af9e004d`。
**只构建，未刷写；HIL 未授权且未执行。**

精确命令、退出码、正式产物 hash、冻结map/UF2及资源清单在
`/tmp/opencode/web-wasm-ab-2026-11-05/validation/`；A/B与性能原始证据在同任务目录
`wasm-experiment/`和`web-experiment/`。不重跑已完成的矩阵来强化结论。

Nix打包的WASM为188347 B、hash `dfd875026435eb0b1f112af94e200b219c81a5ee368b5263bcf7c6f7165c4eb8`，
glue与正式构建相同；因其构建环境不同，不能仅凭Nix检查成功假设与D字节一致。
已完成有界复核：多出的494 B由430 B `.dep-v0`依赖审计节、八条vendor诊断路径增长
24 B及额外零填充40 B组成。审计清单与Cargo.lock一致，没有删除它来强求同hash。
public ABI、17页初始内存、allocator/grow接口与44组完整输出对照通过。

Nix的element/code节仍有同长度字节差异，不能说“只有路径变化”或作全指令等价断言；
未测其运行性能，不把D的量化性能结果直接外推。其+494 B也不混入正式固件的
Flash822340 B或净省13148 B结论。完整归因与等价证据在`validation/nix-equivalence/`。

原index物理hash、cached diff与HEAD保持不变；Nix源文件仅增加批准的decoder命令／
路径hunk，原host/CLI/dependency哈希、锁文件和其他既有工作均保留。改动留在工作树，
本轮没有commit或push。
