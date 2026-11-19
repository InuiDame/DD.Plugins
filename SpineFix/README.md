# SpineFix

修《暗黑地牢》里 spine 2.1 的几处「数据读进来了但画不出来」的功能：

| 功能 | 原版状况 | 本插件 |
| --- | --- | --- |
| 切片显示/隐藏关键帧（slot attachment） | 时间线和 apply 都在 spine-c 里，但绘制前不一定生效 | 每次绘制前按当前轨道时间补一次 |
| 层级调整关键帧（drawOrder） | 同上；`Render` 确实按 `skeleton->drawOrder` 画，但 `drawOrder` 可能停在初始顺序 | 同上，按帧里的排列重排 `drawOrder` |
| 图像特殊效果（加法 / 正片叠底 / 滤色） | `Render` 一个 sprite 只用一个混合状态，从不读 slot 的 additive 字节 | 按混合模式把 `drawOrder` 切成连续段，每段换一次混合状态各画一遍 |

插件挂在 `SpineSprite::Render`（`2585E0`）入口，所有修改都在绘制前发生，不碰顶点、UV、贴图、骨骼计算。

## 混合模式约定

混合模式就是 `.skel` 里 slot 记录最后那个 additive 字节（原版 2.1 只有 0/1）：

| 值 | 含义 | GL 混合（贴图是预乘的） |
| --- | --- | --- |
| 0 | 正常 | 沿用调用方的状态（`ONE / ONE_MINUS_SRC_ALPHA`） |
| 1 | 加法 | `ONE / ONE` |
| 2 | 正片叠底 | `DST_COLOR / ONE_MINUS_SRC_ALPHA` |
| 3 | 滤色 | `ONE / ONE_MINUS_SRC_COLOR` |

这个映射不是凭空定的：拿游戏目录加创意工坊的 9518 个 `.skel` 统计，字节 2/3 已经有人在用，切片名基本是
`shadow`、`silhouette`、`Body_Skirt1_Shadow`（要压暗的阴影）和 `guanghuanda*`、`texiao*`（要发光的光环），
和乘法 / 滤色的语义对得上。原版忽略这个字节，所以这些切片现在都是按正常混合画的。

### 改 `.skel` 的混合字节

`tools/set_blend.py` 只改这一个字节，文件长度不变：

```powershell
python tools/set_blend.py --list hero.sprite.combat.skel          # 报告每个切片的当前值
python tools/set_blend.py --set Body_Skirt1_Shadow=2 hero.sprite.combat.skel            # 预览
python tools/set_blend.py --set Body_Skirt1_Shadow=2 hero.sprite.combat.skel --write    # 写回（留 .blendbak）
python tools/set_blend.py --all 2 hero.sprite.combat.skel --write
```

脚本会先在内存里解析到 slots 段、核对名字可读和骨骼下标合法，解析对不上就直接跳过，不会写坏文件。

## 配置

`<游戏根目录>\DDModLoader\cfg\SpineFix.cfg`，第一次运行自动生成：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `repair_timelines` | `1` | 0 不动，1 修补，2 只记录差异不修改（用来看原版到底补没补） |
| `blend_modes` | `1` | 是否支持混合模式 |
| `additive` | `1` | 字节为 1 时是否按加法画。原版有些特效切片带 1，觉得变亮了就设 0 |
| `max_passes` | `8` | 一个 sprite 最多拆几段；超出的并进最后一段（层级仍按 drawOrder） |
| `verbose` | `0` | 每次修补都写日志 |
| `probe` | `0` | 每种骨骼资源第一次绘制时打一份自检：槽位数、混合字节分布、当前动画的时间线类型 |

自检那行长这样，用来确认资源有没有被读进来：

```
probe: slots=23 blend normal/add/mul/screen/other=23/0/0/0/0 animation="show" timelines=45 attachment=0 drawOrder=0 color=21
```

## 日志

`<游戏根目录>\DDModLoader\log\SpineFix.log`。修补只在预算内记明细（默认 24 次），之后只累计：

```
repaired "idle": 1 attachment, 1 drawOrder (total 12 / 12)
multi-pass draw: 3 pass(es)
```

## 验收状态

- [运行] 特征码在基准 exe 上命中 `2585E0` / `853330` / `8559A0`，RVA 校验一致；测试目录（`rig.py` 的 scenario4）跑 100 秒不崩，日志停在 hook 安装和一条 probe。
- [运行] `tools/set_blend.py` 对游戏目录 + 创意工坊 9518 个 `.skel` 全部解析成功（2.1.27 / 2.1.08 / 抹掉版本号三种都有）。
- [运行] `spinefix_test` 60 项离线检查通过：帧查找边界、显示/隐藏、排列与尾部补齐、多轨道、分段与段数上限。
- 未验证：带混合字节 2/3 的资源实机画面；attachment / drawOrder 修补的真实命中（原版内容里没遇到这两类关键帧，
  需要装一个用了它们的 mod，把 `probe` 和 `verbose` 打开看日志）。

## 限制

- 只处理当前条目（`sprite+0x0C` 指向的那份 `.skel`），和原版 `Update` / `Render` 一致。
- 只修混合与时间线，不补原版其它绘制限制：多页图集只用第一页、非预乘贴图的边缘、附件自身颜色不参与。
- 和 `SpineCompat` 抢同一个 hook 目标（都 detour `2585E0`），两者不要同时装。
- 混合段数越多，一只 sprite 的 draw call 越多；原版资源几乎全是 0，只有明确写了 2/3 的切片才会拆。

## 构建

```powershell
cd DD.Plugins
xmake build SpineFix          # 产物 build\windows\x64\releasedbg\SpineFix.dll
xmake build spinefix_test     # 离线检查，默认不构建
```

设置 `DDPath` 时构建后自动复制到 `mods\DD Mod Lib\`。

## 代码结构

| 文件 | 内容 |
| --- | --- |
| `src/SpineLayout.h` | spine-c 2.1 结构视图和偏移，`static_assert` 守住 |
| `src/Timeline.cpp` | 帧查找、attachment / drawOrder 修补（纯逻辑，可离线测） |
| `src/Fix.cpp` | 混合字节读取、按 drawOrder 切段 |
| `src/Hooks.cpp` | 特征码、detour、分段绘制、日志 |
| `src/Gl.h` | 用到的 GL 入口与混合状态保存/恢复 |
| `src/Config.cpp` | `SpineFix.cfg` 读写 |
| `tools/test_timeline.cpp` | 假结构体驱动的离线检查 |
| `tools/set_blend.py` | 改 `.skel` 的混合字节 |
