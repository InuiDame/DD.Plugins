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
| `probe` | `0` | 每种骨骼资源第一次绘制时打一份自检：槽位数、混合字节分布、当前动画与**整份骨骼所有动画**的时间线统计 |

自检那行长这样，用来确认资源有没有被读进来。竖线后面是**扫描全部动画**的结果：
`all:` 下面是动画总数、attachment / drawOrder 时间线总数；有 drawOrder 的动画会把名字和条数列出来，
不需要正好播到那一段就能判断资源里有没有层级关键帧。

```
probe: slots=23 blend normal/add/mul/screen/other=23/0/0/0/0 animation="show" timelines=45 attachment=0 drawOrder=0 color=21 | all: anims=2 attachment=0 drawOrder=0
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
- [运行] 2026-11-20 测试员实机反馈：**图层混合效果（加法 / 正片叠底）已经能正确使用**，对应 Eos 角色资源里
  `Body_Skirt1_Shadow` 这类切片。同时确认：**带图层顺序关键帧的资源原本会崩溃，装上插件后不再崩溃**。
- 未通过：图层顺序关键帧的画面仍未达到 Spine 编辑器里的效果。当次 `SpineFix.log` 里一条 `repaired …` 都没有，
  说明绘制前的 attachment / drawOrder 修补没有命中，问题在「时间线有没有进入动画对象」这一层，不在补得对不对。
  测试员认为这项不重要，暂不深挖。2026-11-20 第二份 probe 日志里 15 份骨骼的 `drawOrder` 全为 0，
  其中一份 `idle` 有 14 条 attachment 时间线但同样没有修补命中，且这批资源的混合字节全是 0——
  说明那次运行没有渲染到出问题的那份资源。已把 probe 扩成扫描整份骨骼的所有动画（新增 `all:` 段），
  下次带 `probe=1` / `verbose=1` 复现即可区分「资源里没有 drawOrder 时间线」和「有但没生效」。

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
