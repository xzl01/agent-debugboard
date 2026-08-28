# 版本管理与发布门禁

[English](versioning.md)

根目录的 [`VERSION`](../../VERSION) 是项目发布版本的唯一数据源。请不要
手动修改 Cargo、npm、WASM 或 Nix 中的版本字段。

## 更新版本

在仓库根目录执行：

```sh
python3 scripts/version_sync.py set 0.3.0
python3 scripts/version_sync.py check
```

`set` 命令会更新所有受管字段，然后执行与 CI 相同的一致性检查。版本号
必须是不带 `v` 前缀的 SemVer；支持 `0.3.0-rc.1` 这类预发布版本。写入
固件版本前，`set` 会执行与 CI 一致的 Zephyr 可表示性校验：三个数字字段
必须为 `0..255`，`VERSION_TWEAK` 必须为 `0`，预发布后缀只能使用小写
`[a-z0-9.-]`。

维护者也可以手动运行 GitHub Actions 中的 **Version Bump** workflow，选择
`dev` 或 `main` 作为目标分支。Workflow 会调用同一脚本，并创建一个只包含
版本同步文件的 Pull Request。

## 受管字段

脚本会同步：

- `VERSION`
- `cmd-ng/Cargo.toml` 和 `cmd-ng/Cargo.lock` 中的 CLI 包版本
- `web/package.json` 以及 `web/package-lock.json` 中的两个根版本字段
- `web/decoder/Cargo.toml` 和 `web/decoder/Cargo.lock` 中的解码器版本
- `nix/package.nix`
- `apps/radxa_linkr_debugger/VERSION`（Zephyr 应用版本）

## CI 与发布行为

Build workflow 会在所有平台编译之前执行版本脚本单元测试和一致性门禁。
任何受管字段与 `VERSION` 不一致时，门禁会列出所有偏差，后续编译任务不会启动。

Release workflow 还会检查发布标签。标签必须严格等于 `v<VERSION>`；例如
`VERSION=0.3.0` 时只允许使用 `v0.3.0` 发布。
