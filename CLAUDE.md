# DD.Plugins

《暗黑地牢》的 C++ DLL 插件工程，用法见 `README.md`。

@../knowledge/README.md

## 工作约定

- 在 guest 上构建和测试（`guest_run`），只认 guest 的 `Darkest.exe`。命令开头先 `source DD.ModLoader/tools/env.sh`，它补上 xmake 路径和用户环境变量，`dd_config` 负责配置平台。要部署进游戏目录时先 `export DD_DEPLOY=1`。
- 游戏运行时 DLL 被占用，部署前先关游戏。
- 新的逆向事实写入 `../knowledge/` 并带可信度标记，这里的 README 只讲用法。
- commit 信息只写主题和正文，不加 Claude 署名。
