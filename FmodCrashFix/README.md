# FmodCrashFix

修复《暗黑地牢》返回主菜单时，FMOD Studio 异步线程读取空指针导致的闪退。

随 [DDModLoader](https://github.com/InuiDame/Darkest-Dungeon-DLL-Mod-Loader) 安装包预装在 `mods\DD Mod Lib\`，由加载器优先且强制加载，没有其他前置，也没有配置项。

其他目录里的同名 `FmodCrashFix.dll` 不会被加载，前置目录的版本优先。只有前置版本加载失败时才会用到它们。

## 排查问题

查看 `<游戏根目录>\DDModLoader\log\FmodCrashFix.log`：

| 日志 | 含义 |
| --- | --- |
| `Patched FMOD 2.3.14.0 at fmodstudio.dll+E2FBF` | 补丁安装成功 |
| `Skipped FMOD dependency without a model` | 拦下了一次原本会闪退的访问，只记录前 32 次 |
| `Incompatible FMOD version` | 特征码不匹配，插件未生效 |
| `fmodstudio.dll is not loaded` | 加载时 FMOD 尚未载入，插件未生效 |

## 实现说明

以下地址基于 `fmodstudio.dll` 2.3.14 (build 164239)，1579520 字节。

### 崩溃路径

主线程在 `Darkest.exe+232540` 按分组卸载 bank，依次调用 `releaseAllInstances`、`unloadSampleData`、`Bank::unload`，再用 `flushCommands` 等待异步线程。异步线程销毁 model 时重新绑定依赖它的条目，访问了持有者为空的 model：

| RVA | 作用 |
| --- | --- |
| `+5EE60` | 注销 model |
| `+30FE0` | 处理该 model 的依赖 |
| `+E2F60` | 遍历容器，重新绑定 ID 匹配的条目 |
| `+86B20` | 解析新的条目 |
| `+85FA0` | 取 `owner+8` 作为 model 查找资源 |
| `+1058F0` | 对 model 做虚函数调用，`this` 为空时在 `+1058FE` 崩溃 |

转储不包含堆内存，持有者的 model 为什么为空没有定论。

### 补丁

补丁位于 `+E2FBF`，即 `+E2F60` 循环里已确认条目匹配、准备调用 `+86B20` 的位置，覆盖 17 字节并跳到动态生成的代码。持有者或其 model 为空时跳到循环的下一项，否则执行被覆盖的指令后返回。

被跳过的条目状态和引用计数都不变。没有选择让 `+1058F0` 返回空：调用方会走到 `+107BD0`，把现存条目改成加载中并为空 model 排队一次加载。

`fmodstudio.dll` 离游戏主模块超过 2GB，DKUtil 的 trampoline 够不到，所以补丁使用 14 字节的绝对跳转。

### 内存布局

| 位置 | 内容 |
| --- | --- |
| `容器+0x10` | 解析用的 ID |
| `容器+0x30` / `+0x38` | 条目数组 / 数量 |
| `条目+0x20` | 持有者 |
| `条目+0x28` | ID |
| `条目+0x38` | 状态，0 表示需要重新绑定 |
| `条目+0x48` | 引用计数 |
| `持有者+0x8` | model |
| `持有者+0x28` | 条目链表头 |

### 验证

`tools/test_patch.cpp` 不启动游戏，用真实的 `fmodstudio.dll` 和构建出的插件验证补丁，RVA 写死在文件开头，FMOD 更新后需要同步修改：

```powershell
xmake build fmodcrashfix_test_patch
build\windows\x64\releasedbg\fmodcrashfix_test_patch.exe "<游戏目录>\_windows\win64" build\windows\x64\releasedbg\FmodCrashFix.dll
```

### 适配新版本

FMOD 更新后如果出现 `Incompatible FMOD version`，需要重新定位补丁点并更新 `Hooks.cpp` 的特征码。特征码同时校验 `条目+0x20`、`+0x28`、`+0x38` 这几个偏移，`持有者+0x8` 由 `+85FA0` 使用，需要人工确认。
