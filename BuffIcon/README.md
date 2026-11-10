# BuffIcon

为《暗黑地牢》的 buff 增加 `buff_icon` 字段，让 buff 在角色状态栏（tray）上显示独立的自定义图标。

随 [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader) 安装包预装在 `mods\DD Mod Lib\`，由加载器优先且强制加载，不依赖其他前置，也不会出现在 GUI 的启用列表里。

其他目录里的同名 `buff_icon.dll` 不会被加载，前置目录的版本优先。只有前置版本加载失败时才会用到它们。

## 用法

在 mod 的 `shared/buffs/*.buffs.json` 里给 buff 加上 `buff_icon`：

```json
{
	"id": "my_buff",
	"stat_type": "hp_dot_heal",
	"amount": 13,
	"buff_icon": "my_icon"
}
```

### 图标文件

`"buff_icon": "my_icon"` 依次尝试以下文件名，名称不区分大小写，末尾的 `.png` 可写可不写：

1. `overlays/tray_my_icon.png`
2. `overlays/my_icon.png`

查找位置按优先级排列：

1. 定义该 buff 的 mod 自己的 `overlays`
2. 其他本地 mod 的 `overlays`
3. 创意工坊 mod 的 `overlays`
4. 游戏本体的 `overlays`

图标使用 PNG，原版 tray 图标的尺寸是 24×24。

### 显示规则

- 配置了 `buff_icon` 的 buff 会从原来的分组中拆出，单独显示一个图标。
- 悬停时使用该 buff 所属分组的原版 tooltip，内容只统计使用这个图标的 buff。
- 多个 buff 指向同一个文件时合并成一个图标。
- 同一单位上没有配置 `buff_icon` 的同类 buff 仍显示原版图标。

### 支持的 buff 类别

| 类别 | tray 类型 |
| --- | --- |
| 普通增益、减益 | 13、14 |
| 毒、流血、灼烧、压力、位移、愈合 DOT | 6 ~ 11 |
| 永不再犯、决心经验加成、城镇事件 | 1、2、3 |
| 死门效果、反击、战斗修正 | 16、18、20、21 |

痴狂、美德、标记、死门、守护、潜行等图标不由 buff 列表决定，不受 `buff_icon` 影响。技能效果临时生成、没有 JSON `id` 的 buff 也无法配置图标。

### 注意事项

- 含 `buff_icon` 的 JSON 文件允许注释，但不能有尾随逗号，否则整个文件的 `buff_icon` 都会被跳过。
- 同一个图标同时用于增益和减益时，悬停只显示其中一种。

## 排查问题

查看 `<游戏根目录>\DDModLoader\log\buff_icon.log`：

| 日志 | 含义 |
| --- | --- |
| `[MAP] <id> -> <path>` | buff 已映射到图标文件 |
| `Custom icon '...' of buff '...' not found` | 所有位置都找不到图标文件 |
| `JSON parse failed in ...` | 该文件不是合法 JSON |
| `All hooks installed and enabled successfully.` | Hook 安装成功 |
| `Incompatible game version, hooks are not installed` | 特征码或内存布局不匹配，插件未生效 |
| `Added N custom tray icon(s) to overlay` | 进入地牢时图标已注册 |

## 实现说明

以下地址和偏移基于 `Darkest.exe` SHA1 `9db2d73b89aa034e2055df65a139403e18df641f`（19428352 字节）。离线版 `_windowsnosteam\win64\Darkest.exe`（SHA1 `3f133c26312bb667f11105ddf41f92275398a8d3`，19267072 字节）的 RVA 不同，但同一组特征码和内存布局校验都能通过。

游戏的 tray 有 28 种图标类型，只有类型 0（quirk）的数量由图标表 `Overlay+0x1B0` 决定。插件把自定义图标追加到这张表，每个图标占一个类型 0 的子槽位，并用路径哈希作为槽位标识。

### Hook

| 名称 | RVA | 作用 |
| --- | --- | --- |
| `CreateOverlay` | `+6F2DC0` | tray 图标表构造完成后追加自定义图标 |
| `IsIconActive` | `+6FBF50` | 判定自定义槽位是否显示，并把被接管的 buff 从原分组去掉 |
| `ShowTooltip` | `+6FCD60` | 自定义槽位改用 buff 原分组的 tooltip |
| `PlayIconPulse` | `+6FC9C0` | 跳过自定义槽位，原函数在 quirk 分支会访问空指针 |
| `QueryBuffs` | `+5C48B0` | 按当前过滤模式裁剪 buff 查询结果 |

DOT 类型的显示判定直接扫描 buff 表，不经过 `QueryBuffs`，因此由 `IsIconActive` 单独处理；它们的 tooltip 仍走 `QueryBuffs`。

### 调用的游戏函数

| 名称 | 位置 | 作用 |
| --- | --- | --- |
| `CreateImage` | `+385ED0` | 按路径创建图片句柄 |
| `IsListedInGroup` | `+700A00` | 普通增益、减益分组的判定谓词 |
| `ReallocIcons` | 调用点 `+6F3D83` | 图标表扩容 |

### 内存布局

| 位置 | 内容 |
| --- | --- |
| `Overlay+0x1B0` | 图标表 `vector`，元素 0x18 字节：`+0` 标识，`+8` 图片句柄 |
| `Actor+0x78` | tray 状态 |
| `Actor+0x460` | buff 表 `vector`，元素 0x418 字节，`+8` 处是 `Buff` |
| `Buff+0x00` | `stat_type` 枚举 |
| `Buff+0x48` | `amount` |
| `Buff+0xA4` | `id` 的哈希，算法为 `h = h * 53 + c` |

DOT 的 tray 类型与 stat 枚举对应关系：毒 6 → `0x04`，流血 7 → `0x05`，灼烧 8 → `0x58`，压力 9 → `0x22`，位移 10 → `0x23`，愈合 11 → `0x24`。

### 适配新版本

`Hooks::Install` 用特征码定位函数，并用几条写死偏移的指令校验内存布局，任何一项不匹配都只写警告、不安装 Hook。游戏更新后如果出现 `Incompatible game version`，需要重新定位上述函数并更新特征码。
