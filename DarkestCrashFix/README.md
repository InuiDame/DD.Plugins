# DarkestCrashFix

修复 64 位 `Darkest.exe` 的 4 类闪退，共 5 个补丁点。只修改进程内存，不改动 exe 文件。

**贡献者：局师傅**

随 [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader) 安装包预装在 `mods\DD Mod Lib\`，由加载器优先且强制加载，没有其他前置，也没有配置项。

| 补丁点 | 闪退场景 |
| --- | --- |
| A `heirloom` | 自定义祖产货币回城结算 |
| B `no_owner` | 无主 ActorDot：DoT 施法者离场后读档 |
| C `part2` | ActorDot 脏标记：传送重返战斗后残留的 DoT 引用 |
| D `teleport` | 传送 ActorDot：传送重返同一场战斗、尚未读档 |
| E `bufffx` | 多重 BuffFX 叠加 |

## 排查问题

查看 `<游戏根目录>\DDModLoader\log\DarkestCrashFix.log`：

| 日志 | 含义 |
| --- | --- |
| `Patched A (heirloom) at Darkest.exe+2968BF` | 该补丁点已安装，地址随版本变化 |
| `All 5 crash fix sites are patched` | 全部安装成功 |
| `Incompatible game version, nothing is patched` | 有补丁点定位失败，其后每行列出失败的点，插件未生效 |
| `Darkest.exe was already modified by a patch tool` | exe 已被独立补丁工具修改，插件不重复修补 |

## 实现说明

修复方案来自局师傅的《暗黑地牢》64 位主程序通用修复补丁工具包 v2。工具包在 PE 末尾新增节区并改写 exe，这里改成运行时打补丁，定位规则和片段内容保持一致。

- 不写死地址、寄存器或字段偏移，这些都从命中的指令解析，所以能适应新的构建。
- 全有或全无：有任何一处定位失败，或有跳转落在被覆盖指令的中间，就不打任何补丁。
- 片段放在 DKUtil 的 trampoline 里，补丁点写 `jmp rel32`，余下字节填 `nop`。
- 工具包打过的 exe 含 `.dxfs`、`.dxfx` 或 `.zaas*` 节区，插件检测到后不再修补。

与工具包的差异只有一处：E 的地址上限。工具包注释写的是 `0x00007FFFFFFFFFFF`，编码进去的字节却是 `0x007FFFFFFFFFFFFF`，这里按注释的本意修正，超出用户态范围的地址也会被拦下。

### 验证

`tools/test_patch.cpp` 不启动游戏，用真实的 `Darkest.exe` 验证定位、片段生成、全有或全无以及 E 的实际行为。片段放在工具包新增节区的 RVA 上，可以直接和工具包的输出逐字节比对：

```powershell
xmake build darkestcrashfix_test_patch
build\windows\x64\releasedbg\darkestcrashfix_test_patch.exe "<游戏目录>\_windows\win64\Darkest.exe"
```

### 适配新版本

游戏更新后如果出现 `Incompatible game version`，日志会逐条写明失败的补丁点和原因，据此调整 `src/Patches.cpp` 里对应的定位规则。
