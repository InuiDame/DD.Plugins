# DD.Plugins

《暗黑地牢》(Darkest Dungeon) 的 C++ DLL 插件工程。插件由 [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader) 加载，使用 [xmake](https://xmake.io) 构建，Hook 基于 [DKUtil](https://github.com/gottyduke/DKUtil)。

## 插件

| 目录 | target | 功能 | 文档 |
| --- | --- | --- | --- |
| `BuffIcon` | `buff_icon` | 为 buff 增加 `buff_icon` 字段，在状态栏显示独立的自定义图标 | [BuffIcon/README.md](BuffIcon/README.md) |
| `CustomTypeEffects` | `custom_type_effects` | 为 buff / trinket 增加按自定义技能 `.type` 匹配的附加效果触发器（预制功能，部署到 `DD Mod Lib`） | [CustomTypeEffects/README.md](CustomTypeEffects/README.md) |
| `DDShield` | `DDShield` | 独立于血条的护盾核心系统（GPLv3） | [DDShield/README.md](DDShield/README.md) |
| `FmodCrashFix` | `FmodCrashFix` | 修复返回主菜单时 FMOD 异步线程空指针导致的闪退 | [FmodCrashFix/README.md](FmodCrashFix/README.md) |
| `DarkestCrashFix` | `DarkestCrashFix` | 修复主程序的 4 类闪退（贡献者：局师傅） | [DarkestCrashFix/README.md](DarkestCrashFix/README.md) |
| `ScrollWheel` | `scroll_wheel` | 给无原生滚轮的界面补滚轮（**v10：纯滚动 + 无外溢裁剪，不画任何 UI**；事件泵 cave + 独立 tick 线程 + 布局数据补丁 + 连续平滑滚动；`r8` 面板 gate + `param_5` 行号只裁滚出网格上沿的格子；自绘滚动条已移除，饰品库用原生条；v10 已实机验收通过；**v11.2**：修 ASLR 判据（cave 改用运行时 vtable）与补给预翻页（`gate_wheel_by_panel` 面板门控）；独立 `guild_scroll` 开关、默认关闭；20:15 实机验收补给门控 / v10 回归 / 工会滚轮全部通过，见 ScrollWheel/README.md §8.4 与 p31） | [ScrollWheel/README.md](ScrollWheel/README.md) |

## 目录结构

```
DD.Plugins/
├─ BuffIcon/           插件：自定义 buff 图标
├─ CustomTypeEffects/  插件：自定义技能 type 触发器（预制功能）
├─ DDShield/           插件：护盾核心系统
├─ FmodCrashFix/       插件：修复返回主菜单时的 FMOD 闪退
├─ DarkestCrashFix/    插件：修复主程序的 4 类闪退
├─ ScrollWheel/        插件：给无原生滚轮的界面加鼠标滚轮（v11.2，补给/背包已验收 + 工会实验性 + 面板门控，不画 UI）
├─ PluginTemplate/     新插件模板，由 !new.ps1 复制
├─ xmake/              公共构建规则 plugin.lua、Plugin.h 模板、版本资源
├─ xmake.lua           工程入口，自动包含各插件目录
├─ !new.ps1            新建插件
├─ !update.ps1         检查或更新 DKUtil
├─ make-sln-msvc.bat   生成 Visual Studio 解决方案
└─ .github/workflows/  CI：构建并上传 dll 和 pdb
```

每个插件是一个独立目录，包含 `xmake.lua` 和 `src/`（`PCH.h`、`main.cpp` 及其他源文件）。

## 环境准备

| 项 | 说明 |
| --- | --- |
| Visual Studio 2022 / 2026 | 安装「使用 C++ 的桌面开发」工作负载 |
| [xmake](https://xmake.io) | 最新即可 |
| 环境变量 `DDPluginAuthor` | 作者名，写入 `Plugin::AUTHOR` 和 dll 版本信息的 CompanyName。可不设，留空 |
| 环境变量 `DDPath` | 游戏安装目录，例如 `D:\SteamLibrary\steamapps\common\DarkestDungeon`。设置后构建会自动部署（见下文）。可不设 |
| 环境变量 `DKUtilPath` | 本地 DKUtil 仓库路径，例如 `D:\WorkSpace\DKUtil`。不设时需用 `extern` 方式提供 DKUtil |

首次使用先检查 DKUtil 是否可用：

```powershell
.\!update.ps1            # 检查 DKUtilPath，或更新 extern/DKUtil
.\!update.ps1 -Extern    # 把 DKUtil 克隆到 extern/DKUtil
```

## 构建

在 `DD.Plugins` 根目录执行：

```powershell
xmake                      # 构建全部插件
xmake build buff_icon      # 只构建一个插件
xmake f -m debug           # 切换模式：debug / releasedbg（默认）/ release
```

产物在 `build\windows\x64\<模式>\<插件名>.dll`。在插件目录里执行 `xmake` 同样会使用工程配置；需要只以该插件为项目时用 `xmake -P .`。

需要 Visual Studio 解决方案时运行 `make-sln-msvc.bat`。debug 模式的 dll 加载后会等待调试器附加。

## 新建插件

```powershell
.\!new.ps1 -Name MyPlugin                          # 最简
.\!new.ps1 -Name MyPlugin -Mod MyMod -Version 1.0.0
xmake build MyPlugin
```

- `-Name`：插件名，同时作为 xmake target 名、dll 名、`Plugin::NAME` 和日志文件名
- `-Mod`：可选，部署到的 mod 文件夹名，写成 `set_values("dd.mod", "MyMod")`
- `-Version`：可选，默认 `1.0.0`

## 部署

同时满足以下两点时，构建完成后会把 dll 复制到 `%DDPath%\mods\<dd.mod>\`：

- 设置了环境变量 `DDPath`
- 插件 xmake.lua 里有 `set_values("dd.mod", "<mod 文件夹>")`

游戏运行时 dll 被占用，部署会失败，构建前请先关闭游戏。

## 日志

`<游戏根目录>\DDModLoader\log\<插件名>.log`，由模板 `PCH.h` 中的 `LOG_PATH` 决定。
