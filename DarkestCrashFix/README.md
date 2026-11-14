# DarkestCrashFix

修复 64 位 `Darkest.exe` 的 7 类闪退 / 崩溃，共 6 个 hook 补丁点 + 1 处原地改写。只修改进程内存，不改动 exe 文件。

**贡献者：局师傅**（A–E）；F、G 由本工作区补充。

随 [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader) 安装包预装在 `mods\DD Mod Lib\`，由加载器优先且强制加载，没有其他前置，也没有配置项。

| 补丁点 | 闪退场景 |
| --- | --- |
| A `heirloom` | 自定义祖产货币回城结算 |
| B `no_owner` | 无主 ActorDot：DoT 施法者离场后读档 |
| C `part2` | ActorDot 脏标记：传送重返战斗后残留的 DoT 引用 |
| D `teleport` | 传送 ActorDot：传送重返同一场战斗、尚未读档 |
| E `bufffx` | 多重 BuffFX 叠加 |
| F `skillbitmap` | 英雄技能位图遍历崩溃：技能位图基址全局被写成 -1（STL 失效迭代器哨兵，mod 英雄技能过多时触发），`test [rdx], eax` 解引用 -1 闪退 |
| G `widgetstring` | `AllocateWidgetString` 断言 `Out of widget string`：界面字符串池只有 32 KB，mod 英雄技能过多时英雄页要显示的字符串撑爆池子，`Assert Failed` 弹窗后卡死 |

## 排查问题

查看 `<游戏根目录>\DDModLoader\log\DarkestCrashFix.log`：

| 日志 | 含义 |
| --- | --- |
| `Patched A (heirloom) at Darkest.exe+2968BF` | 该补丁点已安装，地址随版本变化 |
| `All 7 crash fix sites are patched` | 全部安装成功 |
| `Incompatible game version, nothing is patched` | 有补丁点定位失败，其后每行列出失败的点，插件未生效 |
| `Darkest.exe was already modified by a patch tool` | exe 已被独立补丁工具修改，插件不重复修补 |

## 实现说明

修复方案来自局师傅的《暗黑地牢》64 位主程序通用修复补丁工具包 v2。工具包在 PE 末尾新增节区并改写 exe，这里改成运行时打补丁，定位规则和片段内容保持一致。

- 不写死地址、寄存器或字段偏移，这些都从命中的指令解析，所以能适应新的构建。
- 全有或全无：有任何一处定位失败，或有跳转落在被覆盖指令的中间，就不打任何补丁。
- 片段放在 DKUtil 的 trampoline 里，补丁点写 `jmp rel32`，余下字节填 `nop`。
- 工具包打过的 exe 含 `.dxfs`、`.dxfx` 或 `.zaas*` 节区，插件检测到后不再修补。

与工具包的差异只有一处：E 的地址上限。工具包注释写的是 `0x00007FFFFFFFFFFF`，编码进去的字节却是 `0x007FFFFFFFFFFFFF`，这里按注释的本意修正，超出用户态范围的地址也会被拦下。

F 是工作区新增的崩溃修复（A–E 来自工具包）。它针对工会 / 英雄页打开时游戏遍历英雄技能位图时解引用 -1 的闪退：崩溃函数入口按全局载入技能位图基址，基址被写成 -1（STL 失效容器哨兵，mod 英雄技能过多时触发）后 `test [rdx], eax` 直接 AV。**F 只做防御——进入即校验位图基址是否为合法用户态指针（0 / 负数 / 过低均判无效），无效时经真实 `ret` 安全返回 0（该技能位判定为“未命中”），并不修复位图全局被写脏的根因**；代价是该技能位可能误判为“未命中”，需实机验收副作用。定位规则用长特征码唯一锁定崩溃函数、并以“前面是 int3 函数边界”“函数体含 `85 02` 崩溃指令”双重校验，避免命中内联进大函数的同名副本。注意被覆盖的 `mov` 是 rip 相对寻址，片段里改用绝对地址读取全局，不能原样重放。

G 是同一场景下的**真正根因**。F 把 -1 哨兵挡住之后，游戏得以继续走到英雄页的字符串装配，然后撞上 `AllocateWidgetString` 的断言：

```
Assert Failed: (( s_widget_strings_allocation_index + source_string_length + 1 ) < s_widget_string_pool_size )
AllocateWidgetString in gui.cpp line 1276 / Out of widget string
```

界面字符串池只是一段 32 KB（`0x8000`）的静态数组，mod 英雄技能一多，英雄页要一次性摆的技能名、说明、状态文字就撑爆了它。这个断言在 release 之外的构建里是直接弹窗卡死，等于另一种闪退。

G 与前六个不同，它不是 trampoline 补丁，而是**原地改写三处**，所以走独立的 `DirectPatch` 通道：

- 池子是一段 `.data` 里的静态数组，全程序**只有一条** `lea rcx,[rip+池]` 以地址方式引用它、且**从不被写入**。`LocateWidgetString` 在运行时逐条验证：rip 引用计数必须为 1，池所在的 32 KB 和索引全局必须完整落在同一个可写节内。任何一条不满足就不打补丁——这是重定位安全的全部前提。
- 把那条 `lea`（`48 8D 0D`，取地址）翻成 `mov`（`48 8B 0D`，取值），池首 8 字节就变成了一个指针槽。
- 启动时 `VirtualAlloc` 一块 1 MB 缓冲区，**先按索引全局的值把旧池里已经写好的字符串搬过去**（安装时机若晚于第一次分配，不搬的话翻转后 `rcx` 会指向字符串字节而不是合法缓冲区；索引大于原池则判定定位错误并整体放弃），然后才把地址写进池首，并把容量检查里的立即数 `0x8000` 抬成 `0x100000`。

改完之后，`AllocateWidgetString`（`Darkest.exe+274BD0`）的容量判定用新上限、基址取新缓冲区，索引全局 `+2A96A88` 的下标语义不变，32 KB 的墙就没了。注意 G 是**扩大容量**而不是像 F 那样“判定无效就跳过”，所以不存在误判副作用；代价只是常驻 1 MB 内存。

### 验证

`tools/test_patch.cpp` 不启动游戏，用真实的 `Darkest.exe` 验证定位、片段生成、全有或全无，以及 E、F 的实际行为和 G 的原地改写。

```powershell
xmake build darkestcrashfix_test_patch
build\windows\x64\releasedbg\darkestcrashfix_test_patch.exe "<游戏目录>\_windows\win64\Darkest.exe"
```

其中 F 的离线断言会先以位图基址 = -1、技能计数 = 1 复现“未打补丁即崩”，再验证打补丁后安全返回 0 且不崩，最后用合法缓冲区确认不阻断正常位图。G 的断言先确认 `lea` / `0x8000` / 池首为零 / 索引全局落位的原始状态，再用手写机器码分别复现“`lea` 取基址”和“`mov` 取指针”两种语义（`rcx = 池基址+索引` vs `rcx = 新缓冲区+索引`）；随后故意在旧池里写入一段“已有字符串”并把索引设成非零，验证 `ApplyDirect` 会搬迁这段内容、翻转 `lea`、抬高上限、写入指针四件事全部落地；最后**回滚**镜像，让后续的定位测试仍能看到原始字节。

### 适配新版本

游戏更新后如果出现 `Incompatible game version`，日志会逐条写明失败的补丁点和原因，据此调整 `src/Patches.cpp` 里对应的定位规则。
