# ScrollWheel — 阶段报告与审核编号索引

按 `DD.ModLoader/docs/06-workflow.md` §10 整理。阶段工作文件仍保留在
`DD.Plugins/build/scrollwheel/`（构建产物目录，不进源码树），本文件是代码与测试中
引用的审核编号 / 阶段编号的权威索引。删除阶段文件时应以本索引为准核对引用。

## 审核编号对照

| 编号 | 阶段报告 | 类型 | 被审核对象 | 主要结论 |
| --- | --- | --- | --- | --- |
| p1-review | `p1-review.md` | 对抗审核 | p2 初版设计的 7 条逆向结论 | 复核事件泵 / 帧清零 / 指令模式 / 虚表槽 / 布局文件 / 全局多写者 |
| p2-review | `p2-review.md` | 对抗审核 | 初版实现方案 H1~H3a | 多写者竞态、cave prolog、虚表签名、裁剪命中跟随；提出低风险替代路径 |
| p4-review | `p4-review.md` | 对抗审核 | v4 首版 DLL | S2 部署误写真实安装、cave 字节、Drain 环形缓冲、返回地址白名单 |
| p4-recheck | `p4-recheck.md` | 复验 | p4 修复 | B1 输入 cave 机器码、B3 CellGate 栈运算、B4 白名单、可执行内存 / 重入 |
| p12-v8-review | `p12-v8-review.md` | 对抗审核 | v8 止血版 | **抓出「v8 从未部署、实机仍是 v7」**、跨线程 owner 风险、滚动条吞点击 |
| p18-v10-review | `p18-v10-review.md` | 对抗审核（初审） | v10 初版 | **B1 CellCaveAsm 三候选整体错位 16B（param_5 从未读到）**、B2 弱位域误判、B3 默认 clip + 下界硬编码 |
| p19-v10-recheck | `p19-v10-recheck.md` | 复验 | v10 初审修复 | B1/B2 消除、`GridEntryAsm` 机器码逐条 capstone 确证、返回 false 无副作用；剩「默认 clip 未经实机验收」一条流程性保留 |
| p23-guild-adversarial-review | `p23-guild-adversarial-review.md` | 对抗审核 | p22 两路工会调研 | 判定「工会 NO-GO」成立，抓 2 阻断 + 6 应修；**其「无注入点」理由后被 p25/p26 推翻** |
| p26-v11-adversarial-review | `p26-v11-adversarial-review.md` | 对抗审核（GO 有条件） | p25 三路调研 + v11 方案 | 亲自反汇编推翻旧 NO-GO 的「无注入点」；给 3 条必须先修：S1 cave 零 `call`、S2 stub 沿用 `GridEntryAsm` 契约、S3 校验整体包在 `if(guildScroll)` 内且失败不 `return` |
| p28-v11-implementation-review | `p28-v11-implementation-review.md` | 实现产物对抗审核 | v11 实际代码与 stub 机器码 | **可交实机验收、无阻断**；应修 1（离开工会残留表）与 2（count 先发布）已修；第三条 DIAG 可观测性记为已知限制 |

## 阶段报告清单（按时间）

| 阶段 | 文件 | 内容 |
| --- | --- | --- |
| P0 | `p0-brief.md` | 范围、验收清单、必读知识库 |
| P1 | `research-a-game.md` / `research-b-loader.md` / `research-c-plugins.md` | 三路并行调研：游戏 / 加载器 / 插件工程 |
| P2 | `p2-design.md` / `p2-design-v2.md` | 初版与重做后的实现方案 |
| P3 | `p3-brief.md` / `p3-progress.md` | 实现简报与进度 |
| P4 | `p4-fix-brief.md` / `p4-fix.md` | 修复简报与报告 |
| P5 | `p5-acceptance.md` | v4 验收清单 |
| P6 | `p6-diagnosis.md` | 「滚轮均不生效」实机诊断 → v5 独立 tick |
| P7 | `p7-layouts.md` / `p7-scrollbar.md` / `p7-fix.md` / `p8-scrollbar-plan.md` | 背包/工会布局数组、动态范围、滚动条方案 |
| P9 | `p9-clip-and-scrollbar.md` | v7 裁剪 + 自绘滚动条 |
| P10 | `p10-clip-research.md` / `p10-count-research.md` / `p10-scrollbar.md` | v7 实机证伪后的三路逆向 |
| P11 | `p11-sackscroll-analysis.md` | SackScroll 1.2 机制逆向（只借鉴，不采用 SDL2 代理） |
| P12~P13 | `p12-v8-review.md` / `p13-v8-report.md` | v8 止血 |
| P14~P17 | `p14-detour-research.md` / `p15-v9-no-scrollbar.md` / `p16-detour-gaps.md` / `p17-provision-dual-chain.md` | detour 可行性、v9 移除滚动条、双链推翻 detour 前提 |
| P18~P20 | `p18-v10-review.md` / `p19-v10-recheck.md` / `p20-v10-clipping.md` | v10 无外溢裁剪 |
| P21 | `p21-v10-acceptance.md` | **v10 实机验收证据**（`p5fail=0`、`skipped` 0→25870、`active` 分布）；发现动态范围实测未生效 |
| P22~P24 | `p22-guild-research-a.md` / `p22-guild-research-b.md` / `p23-guild-adversarial-review.md` / `p24-closeout.md` | 工会 NO-GO（旧结论）+ v10 收尾 |
| P25 | `p25-guild-slots-a.md` / `p25-guild-slots-b.md` / `p25-guild-slots-c.md` | 用户要求继续攻坚工会后的三路重评：技能槽容器 / 命中同源 / 状态机隔离 |
| P26 | `p26-v11-adversarial-review.md` | v11 方案独立对抗审核，**GO（有条件）** |
| P27 | `p27-v11-guild.md` | **v11 实现、验证、部署与实机验收清单**（§7 记录 p28 审核与两处加固） |
| P28 | `p28-v11-implementation-review.md` | 实现产物独立对抗审核：可交实机验收、无阻断；应修 1/2（活跃窗口、count 发布顺序）已落实 |

> **阶段原件位置**：表中 `pNN-*.md` 全部保留在本地 `DD.Plugins/build/scrollwheel/`
> （共 45 份，含实机证伪链、反汇编取证、对抗审核记录），按 `.gitignore` 的
> `[Bb]uild/` 规则**不随仓库提交**。本文件只承担「审核编号 → 结论」的可检索索引；
> 若需完整证据链，请在本地工作副本查阅对应阶段报告。

## 版本备份

| 文件 | 版本 | md5 |
| --- | --- | --- |
| `scroll_wheel.v4.dll.bak` | v4 | — |
| `scroll_wheel.v7.dll.bak` | v7（实机证伪） | `0e4a65d2…` |
| `scroll_wheel.v8.dll.bak` | v8 | `93cbff33…` |
| `scroll_wheel.v9.dll.bak` | v9（只平移不裁剪基线） | `5293c278…` |
| `scroll_wheel.v10.dll.bak` | **v10 已实机验收本体** | `3384bb7a90f31d9292b4d8f9b58abcf9` |
| `scroll_wheel.v10.1.dll.bak` | v10.1 纯文案修订 | `1cfd174b33d1d965be99c919ec0fc939` |
| `scroll_wheel.v11.dll.bak` | v11 实机证伪版 | `844a3876c02b49a237e63b59fe15f962` |
| `scroll_wheel.v11.1.dll.bak` | v11.1 实机证伪版（换捕获链但没修 ASLR） | `a1d3d54a1953600c0c30bc6849a51a2a` |
| 当前部署 | **v11.2（ASLR 修复 + 面板门控；20:15 实机验收：补给 / v10 回归 / 工会 全部 ✅）** | `95f80f3d4b27c2e9765a670cc19d9f91` |

归档代码：`deprecated/ScrollBar.h` / `ScrollBar.cpp`（v7/v8 自绘滚动条，v9 起不再编译）。
离线单测输出副本：`v11_test_run.log` / `v11_test_run2.log` / `v11_final_test.log`（129 PASS）、
`v111_test_run.log`（132 PASS）、`v112_test_run.log`（**139 PASS / 0 failure**）。

| 编号 | 报告 | 结论 | 来源 | 关键发现 |
| --- | --- | --- | --- | --- |
| P29 | `p29-v11-guild-failure-diagnosis.md` | ⚠️ **结论已被 P30 推翻**：误判「捕获链 `0x66E9F0` 从未执行」 | 19:32~19:34 实机日志（116 条 DIAG） | 原文保留作历史；正确解释是 `gBeat=0` 只代表判据没命中。文件头已加醒目更正 |
| P30 | `p30-v112-aslr-and-panel-gate.md` | **★ 真正根因 = ASLR；并修补给预翻页** | 19:48~19:50 实机日志（`gACalls=258` / `gBCalls=196565` / `gVtA=7FF7D8C8B8D8`）+ 算术确证 + 代码审计 | `gVtA-0xEBB8D8=0x7FF7D7DD0000`（64K 对齐 ASLR 基址）；cave 硬编码磁盘 VA `0x140EBB8D8` 永不匹配 ⇒ 改为从表读 `g_base+0xEBB8D8`。另查出 `Wheel::Tick` 的 `layoutHit` 恒 true + 每帧无条件写三套布局 ⇒ 非目标界面滚轮污染补给 `start_y`，新增 `gate_wheel_by_panel` 门控 |
| P31 | `p31-v112-acceptance-and-closeout.md` | **实机验收 + 收尾 + Git 提交准备**；补给门控 ✅ / v10 回归 ✅ / **工会滚轮 ✅（用户确认）** | 20:04:57~20:06:46 单会话 109 条 DIAG + 20:15 用户确认 | `pRecent` 随 `active=2` 开关、非目标界面 `cur=0 / storeY=20`（预翻页已修）、补给内 `cur=87.56 / skipped=312`（v10 回归通过）；runtime vtable 日志 `0x7FF7D8C8B8D8` 确证 ASLR 修复。用户明确「工会翻页完全生效」⇒ 按已验收定稿。日志中工会 DIAG 全 0 与环境解释均已复核排除（仅 1 份 DLL / 1 份日志 / cfg 正确），保留为**已知诊断可观测性缺口**（统计未覆盖实际生效路径），非功能缺陷。含 Git 待提交清单与提交信息 |

## 决策记录

- ADR 0001 `docs/adr/0001-guild-scroll-no-go.md`：**已被 v11 重评取代**。
  原文保留 12:50 的 NO-GO 论证作为历史；末尾「v11 重评」节说明为何「无注入点」
  理由被推翻、新决策（实验性、默认关闭、单独实机验收）与仍存风险。
- 代码与 cfg 中凡出现 `pNN` / `SNN` / `BNN` 编号，均可在上表定位到对应阶段报告；
  阶段文件删除前应把需要的证据段落摘进 `docs/history/`。
