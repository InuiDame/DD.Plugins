# example/ 说明

两套最小示例，分别演示两种参数形式。**这不是可直接安装的完整数据 MOD**，只是数据写法片段，
放进你自己的 MOD 对应路径即可。

## 1. buff 形式：`example.buffs.json`

放到 `shared/buffs/`（或 `dlc/`、`mods/`、workshop 下任意目录），文件名保持 `*.buffs.json`。

- 新增三个布尔字段：`immune_kill` / `immune_control` / `immune_capture`，缺省 `false`。
- 游戏自身会忽略这些未知字段；DLL 扫描同名文件读取它们。
- 同一 buff id 在多个根出现时，高优先级覆盖低优先级（含显式 `false` 覆盖低优先级的 `true`）。
- 运行时只要该 buff 挂在目标身上（`Actor+0x460` 组里存在对应 id 哈希），免疫即生效。

## 2. info 形式：`example.info.darkest`

放到 `heroes/<name>/<name>.info.darkest` 或 `monsters/<name>/<name>.info.darkest`
（示例中的职业名是 `example_class`，请替换为真实职业名）。

- **直接新增真实块** `immune: .kill true .control true .capture true`，与 DDShield 的
  `shield: .max 25 .start 25` 同款，写在文件顶层即可，**不需要 `//` 注释**。
- 也兼容裸 token：`immune: kill, control`（逗号/空白分隔，等价于 true）。
- 显式 `immune: .capture 0` / `.control false` 可清除此前置位；多行按文件顺序处理。
- 大小写不敏感；未知 token 写 WARN 忽略。
- 游戏解析器只在"标识符后面不是 `:`"时报错停止；`immune:` 是合法块（标识符 + 冒号），
  游戏不认识但会忽略，**不会**截断后续的 `resistances:` / `combat_skill:` 等块。
- 为向后兼容，旧写法 `// DD_IMMUNE: ...` 仍可识别，但**推荐真实块**。

## 组合语义

buff 与 info 是**两级 OR**：目标最终 mask = class 级（info 命中） | buff 级（buff 命中）。
例如职业写了 `kill`、buff 写了 `control`，则该目标同时免疫 kill 与 control。
