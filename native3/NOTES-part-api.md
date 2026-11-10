# 部位 API 重构设计（阶段一：只出设计，不动 .cpp/.h）

2026-11-10 21:5x 起。作者：米鱼（子代理跑的设计）。审的人：ljm。

**这一份不改任何代码。** 下面所有行号都是 2026-11-10 21:44 那次启动的源码快照，
审过之后照第二节（推荐方案）× 第五节（迁移步骤）施工。

---

## 一、现状：同一件事散在九个地方（不是六处）

任务书写了六处。我按行号把整条链走了一遍，**实际是九处**，多出来的三处都是今天
一整天踩坑的直接来源（飘带判成 leg、耳朵判成 head、点脸没反应）。

### 1.1 主干六处

| # | 位置 | 职责 | 数据来源 |
|---|---|---|---|
| ① | `src/partpoke.h:58` `kZoneNames[14]` / `:61` `kZoneCount` | 部位名单，**数组下标就是语义** | 写死在 C++ |
| ② | `state/part-bands.tsv`（11 条） + `src/main.cpp:1450-1515` | 判定带（先把格子钉死），带 force/fill 两档 | 表驱动（唯一做对的） |
| ③ | `state/param-parts.tsv`（13 行 P） + `src/partpoke.h:168-186` | 点它拨哪几个物理**输入**、幅度、评分 | 表驱动 |
| ④ | `state/part-react.tsv`（12 行） + `src/web.cpp:600-630` + `src/main.cpp:1826` | 点它切哪个表情（`/` 分隔=随机） | 表驱动（网页可写） |
| ⑤ | `src/main.cpp:3138 legDragTick()` | 拖动时腿跟随（**只给腿写了**） | 全硬编码 |
| ⑥ | `src/platform_wayland.cpp:356-381` | 判定后的分发：`classify` + `hasParams` + `hasExpr` 拼起来决定走哪条路 | 逻辑硬编码 |

### 1.2 我另外找到的三处（今天踩坑的根）

| # | 位置 | 职责 | 为什么要记 |
|---|---|---|---|
| ⑦ | `src/main.cpp:1062 zoneOfPartName()` | drawable 的 part 名（中文）→ 部位下标。**第三份部位名单**，用中文关键词匹配，顺序有讲究（`眼镜` 必须先于 `眼`） | 加一个部位要改这里 20 行 |
| ⑧ | `src/partpoke.h:207` classify 里的 ear 硬判 | `v 0.198..0.248 && (u<0.425 \|\| u>0.575)` → ear，**排在格子图之前、无视剪影** | 和 `part-bands.tsv` 里那两条 ear 带**语义不同**（那两条受 cellOk 限制、且闭区间），同一件事两处写法 |
| ⑨ | `src/web.cpp:343 PARTS`（前端 12 条） | 设置页「点击反应」的部位列表 + 中文标签 | 第四份名单，加部位要改 JS |

### 1.3 三处数据、四种读法（这才是"不同步"的机制）

| 读什么 | 谁读 | 路径怎么找 | 多久读一次 | 改了要重启吗 |
|---|---|---|---|---|
| `param-parts.tsv` | `partpoke.h:130 table()` | `findFile()`：`MIYU_PARTS_FILE` > exe 旁 `state/` > `../state/` > `state/` | 进程内一次（惰性） | 要 |
| `part-zones.tsv` | 同上 `table()` | `findFile()`（`MIYU_ZONES_FILE`） | 一次 | 要 |
| `part-bands.tsv` | `main.cpp:1474` | **裸相对路径** `"state/part-bands.tsv"` | 一次 | 要 |
| `param-range.tsv` | `main.cpp:1719` | **裸相对路径** `"state/param-range.tsv"` | 一次 | 要 |
| `part-react.tsv` | `web.cpp:603` | **裸相对路径** | 启动 + 每次页面改 | **不用**（web 内存镜像，每帧读） |

三个后果：

1. **裸相对路径依赖 cwd。** systemd unit 里没有 `WorkingDirectory`，靠
   `~/.local/bin/miyu-body` 那个 wrapper `cd "$BODY_DIR"` 兜着（进程 cwd 实测
   `/home/link/.dsh/miyu/body/native3`）。**手动 `./build/miyu-wayland` 在别的目录跑，
   四张表全静默读不到**，只有一行日志。
2. **`main.cpp:1458 kFallback` 那份内置兜底只有 9 条，文件是 11 条 —— 少了 skirt 那两条。**
   也就是说：文件读不到时，两条飘带尖端会被判成 leg/tail。**今天「飘带判成 leg」
   的隐患有一半埋在这里**（真正修它的是文件里那两行，而兜底偷偷更旧）。
3. **只有表情是热生效的**（web 内存镜像 + 每帧读），其它四张表都要重起服务。
   这一点在合并成一张表时**必须保住**，否则是回归。

### 1.4 还有两处藏起来的状态

- `src/main.cpp:1864` `gTapExprUntil = now + 5.0`：表情停留 5 秒，**硬编码**。
  全局另有 `web::settings().exprHold`（0.2–5 倍率，基准 `kExprBaseHold=2.5s`），
  是**另一条路**（ctl 的 `expr` 命令），和点击表情没关系。
- `src/main.cpp:1699` `upOnly = (part == "mouth" || part == "brow")`：嘴/眉只往正方向。
  这条也是「部位属性」，现在是 if 里认名字。
- `src/main.cpp:1893-1899` 与 `:1900` `follow`：`legSway`/`chestSway` 两个档位只影响
  弹簧刚度，**`legSway` 压根没落盘**（`saveSettings()` 里没有它，`web.h:72` 有字段、
  页面有滑块、读回没分支）—— 重启回 1.0。顺手记这里，迁移时一起修。

### 1.5 耦合图（谁依赖谁）

```
                         ┌────────────────────────────┐
 drawable 的 part 名 ───→│ ⑦ zoneOfPartName (中文关键词) │─┐
                         └────────────────────────────┘ │
                                                        ↓ 下标
   part-zones.tsv ──→ table().zones ──┐      ① kZoneNames[14] ←── ⑨ 前端 PARTS
                                       ↓                ↑
   渲染层 partZoneTick 光栅化 → 格子图 → ② applyBands 改格子 → gZoneCells
                                       ↓（写入 inline 邮筒）
                    平台层 classify(u,v) ──→ 部位名 ──→ ⑥ 分发
                                                          ├─ ③ hasParams(部位) → partPokeStart（拨 Poke*）
                                                          └─ ④ tapExprFor(部位) → exprPlay + 5s
                                                          ↓
                                              ⑤ legDragTick（只 leg，且看 gPartPoke.part=="leg"）
```

**危险点**：`gZoneCells` 存的是 `unsigned char` 下标，**下标即部位**。所以
`kZoneNames` 只能往后加（`partpoke.h:57` 那句注释就是这么写的）。这套东西不能
原地重排，迁移必须保序。

---

## 二、四个方案

### 方案 A：一张 `state/parts.tsv`（多记录类型）+ 一个 header-only Loader ★推荐

一份文件，行首是记录类型（`part` / `alias` / `band` / `param` / `expr` / `follow`），
每行都带部位名。`src/partapi.h`（header-only，inline，**不动 CMakeLists**）一次读完，
给出 `classify / applyBands / paramsOf / hasAction / pickExpr / followTick / nameOf / labelOf`。

- **优点**
  - 「加一个部位」= 只改这一个文件（part + alias + band + param + expr 各加几行）。
    `kZoneNames`、`zoneOfPartName`、前端 `PARTS` 全部由它生成，C++ 一行不用改、不用重编译。
  - 优先级显式：`force/fill` + `prio` 数字 + `scope=body|any` 三个维度，都不再靠行序
    （只有同 prio 才看行序，与现状逐位一致）。
  - 一屏能看完整块身体：改一个部位的带和它的动作，不用在三个文件间跳。
  - 只有一处解析代码，错一行报行号，能写自检（`MIYU_PARTS_DUMP=1` 启动即打印归一化结果）。
  - header-only ⇒ CMakeLists 两个 target 的源文件列表都不用动（本轮同一条约束，`partpoke.h:3-4` 也是这么躲的）。
- **缺点**
  - 文件类型混杂，一个文件里六种行；新人要先读表头。
  - web 写回表情时**要重写整个文件**（只能保行、不能保格式），是全套里唯一有写入风险的路径。
- **改动面**：新增 `src/partapi.h` + `tools/parts-migrate.py` + `tools/parts-dump.py`；
  改 `main.cpp` 4 处（band 读、zoneOfPartName、tapExprFor、legDragTick）、
  `partpoke.h`（`classify/top/hasParams/kZoneNames` 改成转调 API）、
  `platform_wayland.cpp` 3 行（`hasParams||hasExpr` → `hasAction`）、
  `web.cpp` 3 处（PARTS、loadPartReact/savePartReact 改成读写 expr 段、/state 多下发 parts 列表）。
  **CMakeLists 不动。**
- **风险**：中（一个解析器 + 一个写回路径）。风险集中在 web 写回，可隔离（见 5.5）。

### 方案 B：保持多文件，一个 Loader 统一读

`state/parts.tsv` 只放部位元信息（name/label/hold/flags），band/param/expr/follow
各自一个文件，`partapi.h` 按固定顺序把四份合成一张 Registry。

- **优点**：每份文件 diff 干净、意图单一；`part-react.tsv` 写入路径完全不动（零风险）；
  迁移可以一份一份来。
- **缺点**：**加一个部位要动 4–5 个文件** —— 直接违背「只需改配置文件」这个目标的原意
  （虽然技术上不重编译）。跨文件的顺序/优先级要靠约定，新的坑会长在「这条 band 该写哪个文件」上。
- **改动面**：与 A 相同，外加 4 个文件的表头与文档。
- **风险**：低（技术），高（体验：下一轮还是会在 4 个文件之间漂）。

### 方案 C：编译期生成（CMake 把 tsv 生成成 .inc 常量）

`custom_command` 把三张 tsv 转成 `parts_gen.inc` 编进去。

- **优点**：运行时零解析、零路径问题、类型安全、错在编译期就报。
- **缺点**：**直接违反「加部位不用重编译」这条硬要求**。
- **改动面**：CMakeLists 两处 target、新增 generator 脚本、`build/` 依赖关系。
- **风险**：不合适，仅作对照 —— 如果以后「不用重编译」这条放宽了，它是解析成本最低的；
  现在排除。

### 方案 D：一张 `state/parts.json`（JSON 总表）

- **优点**：天然嵌套（部位 → 数组），工具生态好，`python3 -m json.tool` 就能校验。
- **缺点**：手改一行 `"u0": 0.330,` 和改 tsv 一行的心智负担差一个数量级；`git diff` 噪声大；
  要新写一个迷你 JSON 解析（`web.cpp:parseFlat` 只吃扁平对象，嵌套用不了）；
  他今天一天改的都是「改一行立刻生效」，JSON 把这条手感打掉。
- **改动面**：同 A，外加 JSON 解析器。
- **风险**：手感风险 > 技术风险。排除。

---

## 三、推荐：方案 A

理由是三条硬要求的乘积：

1. **「加一个可点部位只改配置文件」** —— 只有 A 能在**一个文件、一次编辑**里完成。
   B 要动 4 个文件；C 要重编译；D 改一行更麻烦。
2. **「不能有三份各读各的」** —— A 是物理上只有一份，B 只是逻辑上统一（读的还是四份）。
3. **「加载失败要有兜底」** —— A 把兜底从「一份和文件不同步的内置表」改成
   **「空表是合法状态」**：读不到 → `classify` 恒空 → 分发退回整套 plan（这条路径
   `platform_wayland.cpp:397-408` 本来就在）。**今天 kFallback 只有 9 条、比文件少两条
   的隐患就此消失**，不是靠维护两处一致，是靠取消第二处。

技术风险（web 写回）单独隔离：写回只碰 `expr` 行，写前重新解析校验，失败不落盘（见 5.5）。
其余记录本来就是「改文件 + 重起服务」的节奏，和今天一致。

ponytail 式的减法，这次做了这些（写在这里免得被当成遗漏）：

- **不写「兼容旧三表」的双格式解析器**。迁移是一次性的（`tools/parts-migrate.py`），
  新旧格式共存只会长出一棵永久双分支。旧表归档到 `state/legacy/`，不参与运行。
- **不加「部位继承 / 模板 / 宏展开」**。13 个部位、36 行数据，手写就行。真到 30 个部位再说。
- **不加缓存层**。进程内一次读完，注册表是 `const`，没有失效逻辑 —— 现状也是「改完要重启」。
  唯一的例外是表情热生效，那用一层显式覆盖（不是缓存）。
- **`part-zones.tsv` 不进合并**。那是渲染层**每帧量出来的运行时数据**，不是配置，
  性质不同，继续留在原地（只是索引改成从注册表取）。

---

## 四、推荐方案的完整数据格式

### 4.1 文件：`state/parts.tsv`

TAB 分列，`#` 开头是注释，空行忽略。**列数按记录类型固定**（多写的列忽略，少写的列取缺省，
缺列时打一行 WARN 但不丢整条记录）。

```
# ── 记录类型一览 ───────────────────────────────────────────────
# part    部位名 中文标签 停留秒 开关
# alias   部位名 中文关键词（逗号分隔，按文件顺序匹配，先出现的赢）
# band    部位名 u0 u1 v0 v1 force|fill 优先级 [body|any]
# param   部位名 参数Id 评分 幅度 alt|up
# expr    部位名 表情名[/表情名…] [停留秒]
# follow  部位名 参数Id 系数 上限 ax|az|ay
# ────────────────────────────────────────────────────────────────
# 部位名 = ASCII，格子图/日志/工具/设置页全用它，永不改动。
# 部位 id = part 行出现顺序（1 起，0 保留给「空白」）。
#   ★ 已有部位的相对顺序不许变，新部位只能往后追加 —— 和现在 kZoneNames 的规矩同一条。
# 中文标签只给人看（设置页 / 日志），改它不影响任何判定。
```

#### part（每个可点部位一行）

```
part	head	头	5.0
part	chest	胸	5.0
...
```
- `停留秒`：点这个部位时表情留多久（现在是硬编码 5.0，`main.cpp:1864`）。空 = 5.0。
- `开关`：逗号分隔，可选值 `dragonly`（这个部位的动作**只在拖动时**生效，点它只放表情）。
  缺省空。这就是要求里的「是否只在拖动时生效」。

#### alias（drawable 的 part 名 → 部位）

```
alias	head	头,脑,发,脸,五官,眼,眉,嘴,鼻,耳,脖,牙,呆毛,眼镜,表情,汗,泪,口内
```
- 顺序即优先级（`眼镜` 那条必须排在 `眼` 前面 —— 现在 `main.cpp:1067` 就是这么做对的）。
- 匹配方式是 `find()` 子串包含（和现在一致），不放正则：作者那些「组 N」「文件夹」靠它落空。

#### band（判定带，0..N 条）

```
band	ear	0.000	0.425	0.198	0.248	force	100	any
band	ear	0.575	1.000	0.198	0.248	force	100	any
band	eye	0.415	0.590	0.208	0.228	force	100
...
```
- `mode=force`：无条件写这格（覆盖已有标签）。
  `mode=fill`：只写「剪影内且还没标签」的格子。
- `prio`：同一格被多条带命中时，**数字大的赢**；数字相同则文件里**靠后**的赢
  （= 现状：force 之间后写的覆盖先写的）。
- `scope=body`（缺省）：只在剪影内（`cellOk`）生效。`scope=any`：**无视剪影、排在一切之前**
  —— 现在 `partpoke.h:207` 那段 ear 硬判就翻译成上面头两行。
- 边界语义**必须写死并照现状**：`u/v` 两端都是**闭区间**（`main.cpp:1505-1510` 是
  `gy <= gy1`），和 `part-zones.tsv` 的 `Z` 行**半开**（`gv < z.v1`，`partpoke.h:234`）
  不一样。这是现存的坑，迁移照抄闭区间，不"顺手统一"——统一会让每条带的边缘多/少一行格子，
  验收数字就对不上了。

#### param（动作参数，0..N 条）

```
param	chest	PokeChest	90	1.000
param	arm	PokeArmL	85	1.000
param	mouth	PokeMouth	94	1.000	up
param	brow	PokeBrow	93	1.000	up
```
- `评分`：比值×10，**同一部位内按它降序**，一次点最多取前 4 个（`partpoke.h:246 top()`）。
- `幅度`：绝对偏移量（不是百分比），正负每点一次交替。
- `alt|up`：`up` = 只往正方向（现在 `main.cpp:1699` 认 `mouth`/`brow` 两个名字）。
- **部位有 0 条 param 是合法的**（head 现在就是 0 条：点头不该整块转，`NOTES` 二十五）。

#### expr（表情，0..N 条，可随机）

```
expr	head	前倾	5.0
expr	brow	黑脸
expr	chest	脸红
expr	ear	＞＜
expr	eye	爱心眼
expr	headwear	流泪
expr	mouth	＞＜
expr	waist	圈圈眼
expr	arm	
expr	leg	
expr	skirt	
expr	tail	
```
- 一条 expr 行的第二列写 `脸红/兔牙/＞＜` = **每次随机抽一个**（现成语法，`main.cpp:1830-1846`）。
- 同一部位写**多条** expr 行 = 候选池合并后随机（数组追加即可，两行代码）。
- 第三列覆盖 part 行的停留秒。
- 第二列空 = 这个部位只放动作、不放表情（现在四条空行照抄）。
- **热生效**：这一项由 web 在内存里覆盖，改完立刻生效、不用重启（见 5.5）。

#### follow（拖动跟随，0..N 条）

```
follow	leg	HipAngleX	0.6	30	ax
follow	leg	HipAngleZ	2.2	30	az
follow	leg	PokeLeg	0.0625	1	ax
```
- `ax/az/ay` = 拖动摆动量（`gSway.ax/az`，量级 ±16 / ±10）。
- 写**绝对值**（不是累加），和现在一致。
- **通用规则**（写在 API 语义里，不是数据里）：**这个部位自己的 poke 弹簧活着时，它的
  follow 整段让位** —— 现在写死 `gPartPoke.part == "leg"`（`main.cpp:3155`），改成通用后
  任何部位都能有 follow。
- 部位有 0 条 follow = 拖动时不跟随（现在除腿以外全是）。

### 4.2 全部 36 行逐行翻译（不是示意图）

现三张表实际条数：**13 行 param + 11 行 band + 12 行 expr = 36 行**，全部在下面。
另外 13 条 `part` 行、13 条 `alias` 行、3 条 `follow` 行是**新增的元信息**
（`part`/`alias` 是今天散在 `main.cpp:1062` 和前端 JS 里的东西，`follow` 是 `legDragTick` 里的三个常量）。

#### 4.2.1 part（13 个命名部位，顺序 = id；数组里共 14 个槽，0 号是空）

```
part	head	头	5.0
part	chest	胸	5.0
part	arm	手	5.0
part	skirt	裙	5.0
part	leg	腿	5.0
part	tail	尾	5.0
part	other	其他	5.0
part	eye	眼	5.0
part	mouth	嘴	5.0
part	brow	眉	5.0
part	ear	兽耳	5.0
part	waist	腰	5.0
part	headwear	头饰	5.0
```

等等 —— `other`（下标 7）在 `kZoneNames` 里有，但**不在** `part-bands.tsv` / `param-parts.tsv` /
`part-react.tsv` / 前端 `PARTS` 里。它只被 `zoneOfPartName` 返回 0 时用（`state/part-zones.tsv`
里没有任何一行是 other，实测 `wc` 过）。所以：

- **要写进去**（保持 id 序列逐位一致：1..13 一模一样，加第 14 个才是新部位）；
- 但**不翻 `label`**（给它 `其他`），且 `band/param/expr` 全空 —— 它存在的意义只是
  「这格有东西但认不出」，点上去 `hasAction=false` → 退回整套反应，和现在一样。

这 13 条正是为了让 **id 1..13 与现行 `kZoneNames` 完全同序**（这是迁移可逐位对账的前提）：

| id | 1 | 2 | 3 | 4 | 5 | 6 | 7 | 8 | 9 | 10 | 11 | 12 | 13 |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| 名 | head | chest | arm | skirt | leg | tail | other | eye | mouth | brow | ear | waist | headwear |

#### 4.2.2 alias（13 条，从 `main.cpp:1062-1090` 原样搬，顺序就是它的 if 顺序）

```
alias	head	眼镜
alias	headwear	女仆发饰,发饰
alias	ear	耳朵,兽耳
alias	eye	眼眶,眼球,眼白,左眼,右眼,圈圈眼,爱心眼,屑屑眼,眼
alias	mouth	嘴,口内,牙,舌
alias	brow	眉毛,眉
alias	waist	腰部
alias	tail	尾巴,狗尾草
alias	leg	腿,脚,膝
alias	skirt	裙,蝴蝶结,缎带
alias	arm	手臂,大臂,小臂,手腕,袖子,举手,竖食指,手
alias	chest	胸,上身,领子,肩膀,腰
alias	head	头,脑,发,脸,五官,眼,眉,嘴,鼻,耳,脖,牙,呆毛,眼镜,表情,汗,泪,口内
```

两条要注意：

- `眼镜 → head` 单独一条排在 `eye` 前面，等价于现在 `main.cpp:1067` 那句「眼镜是 head，不是眼睛」。
- 最后一条 `head` 是 catch-all，里面重复了 `眼/眉/嘴/耳/眼镜/口内` —— **现在就是这样**
  （`main.cpp:1083-1087`），前面的精确匹配先赢，重复项只是「兜到 head 也不会错」。
  迁移时**原样保留**，别顺手去重（去重会改掉「作者把某个东西命名成『眼睛袋』」这类边角的落点）。
- `chest` 那条里的 `腰`（宽匹配）和 `waist` 那条的 `腰部`（窄匹配）：顺序上 `腰部` 在前，
  所以「腰部衣服」归 waist、「腰带」归 chest —— 和现在一致。

#### 4.2.3 band（11 条，从 `state/part-bands.tsv` 逐行）

| 原行（部位 u0 u1 v0 v1 强制 说明） | 新行 |
|---|---|
| ear 0.330 0.405 0.195 0.245 1 兽耳左 | `band	ear	0.330	0.405	0.195	0.245	force	100` |
| ear 0.595 0.680 0.195 0.245 1 兽耳右 | `band	ear	0.595	0.680	0.195	0.245	force	100` |
| eye 0.415 0.590 0.208 0.228 1 眼睛 | `band	eye	0.415	0.590	0.208	0.228	force	100` |
| brow 0.430 0.575 0.186 0.208 1 眉毛 | `band	brow	0.430	0.575	0.186	0.208	force	100` |
| mouth 0.450 0.560 0.236 0.282 1 嘴和下巴 | `band	mouth	0.450	0.560	0.236	0.282	force	100` |
| waist 0.358 0.592 0.432 0.506 1 腰那一圈 | `band	waist	0.358	0.592	0.432	0.506	force	100` |
| skirt 0.690 0.800 0.500 0.630 1 右飘带尖端 | `band	skirt	0.690	0.800	0.500	0.630	force	100` |
| skirt 0.300 0.400 0.550 0.630 1 左飘带尖端 | `band	skirt	0.300	0.400	0.550	0.630	force	100` |
| headwear 0.395 0.470 0.118 0.178 0 女仆发饰左 | `band	headwear	0.395	0.470	0.118	0.178	fill	0` |
| headwear 0.535 0.610 0.118 0.178 0 女仆发饰右 | `band	headwear	0.535	0.610	0.118	0.178	fill	0` |
| head 0.400 0.600 0.090 0.205 0 头顶到眉线 | `band	head	0.400	0.600	0.090	0.205	fill	0` |

**外加两条**（把 `partpoke.h:207` 那段硬判搬进表，`scope=any`）：

```
band	ear	0.000	0.425	0.198	0.248	force	200	any
band	ear	0.575	1.000	0.198	0.248	force	200	any
```

`prio=200` + `any`：命中的是「耳朵窄带、脸中间的格子不动」，无视剪影 —— 和现在
`classify()` 第一步就 `return "ear"` 逐位等价（唯一的差别：现在是 `v <= 0.248` 闭区间，
格子是半开；用 `any` 直接在浮点 u/v 上比，闭区间照抄，差一格的问题不存在）。

#### 4.2.4 param（13 行，从 `state/param-parts.tsv` 逐行）

```
param	chest	PokeChest	90	1.000
param	arm	PokeArmL	85	1.000
param	arm	PokeArmR	84	1.000
param	skirt	PokeSkirt	80	1.000
param	leg	PokeLeg	88	1.000
param	tail	PokeTail	92	1.000
param	eye	PokeEye	95	1.000
param	mouth	PokeMouth	94	1.000	up
param	brow	PokeBrow	93	1.000	up
param	ear	PokeEarL	91	1.000
param	ear	PokeEarR	91	1.000
param	waist	PokeWaist	90	1.000
param	headwear	PokeHeadWear	90	1.000
```

`up` 只加在 mouth / brow（现在 `main.cpp:1699` 的 `upOnly`）。
head **0 条**（二十五拿掉了整组）—— 这条「没有」本身也要在文件里体现（靠 absence，
不写占位行）。

原始备注（「胸X 的局部版」「嘴：ParamMouthOpenY 自己拼的一条链」……）逐条搬到文件里
**行尾注释**（`# ` 之后），别丢 —— 那是二十三/二十四节里量出来的教训。

#### 4.2.5 expr（12 行，8 个非空）

```
expr	head	前倾	5.0
expr	chest	脸红	5.0
expr	brow	黑脸	5.0
expr	ear	＞＜	5.0
expr	eye	爱心眼	5.0
expr	headwear	流泪	5.0
expr	mouth	＞＜	5.0
expr	waist	圈圈眼	5.0
expr	arm
expr	leg
expr	skirt
expr	tail
```

`＞＜` 是 U+FF1E + U+FF1C 全角（**不要**写成 `<` `>`）。中文标签/表情名里的全角字符
是被"感觉"改坏的高危区，迁移脚本要按字节比（`diff` 原文），不按肉眼。

#### 4.2.6 follow（3 行，从 `legDragTick` 的三个常量）

```
follow	leg	HipAngleX	0.6	30	ax
follow	leg	HipAngleZ	2.2	30	az
follow	leg	PokeLeg	0.0625	1	ax
```

原式：`hipX = ax*0.6` clamp ±30、`hipZ = az*2.2` clamp ±30、`pk = ax/16` clamp ±1。
`1/16 = 0.0625`。

### 4.3 API 形状（`src/partapi.h`，header-only）

```cpp
namespace partapi {

struct Band   { unsigned char id; float u0,u1,v0,v1; bool force; int prio; bool any; };
struct Param  { unsigned char id; std::string name; int score; float amp; bool upOnly; };
struct Expr   { unsigned char id; std::vector<std::string> picks; double hold; };
struct Follow { unsigned char id; std::string name; float gain, clamp; char src; };

// 进程内只读一次。失败 → 空注册表（合法状态，不是 fatal）
const Registry& reg();

// ── 渲染层（main.cpp 的 partZoneTick 里）──
// 在 alpha 白名单之后、泛洪补洞之前调，按 prio 应用 scope=body 的带
void applyBands(std::vector<unsigned char>& grid, int gw, int gh,
                const std::vector<bool>& inSilhouette);

// ── 平台层（platform_wayland.cpp 的 tap）──
std::string classify(float u, float v);        // scope=any 带 → 格子图 → Z 行兜底
bool        hasAction(const std::string& part);// params 非空 或 exprs 非空（取代 hasParams||hasExpr）

// ── 渲染层（partPokeTick / 拖动）──
std::vector<Param> paramsOf(const std::string& part);      // 按 score 降序
bool  pickExpr(const std::string& part, unsigned seed,
               std::string* out, double* hold);            // false = 这个部位不放表情
void  followTick(int model, double now);                   // 读全部 follow 行 + gSway
const char* nameOf(int idx);                               // 取代 kZoneNames[]
const char* labelOf(int idx);
std::vector<std::pair<std::string,std::string>> allParts(); // 设置页下发（取代前端 PARTS）

// ── web 写回（表情热生效）──
void  setExprOverride(const std::string& part, const std::string& val); // 内存覆盖
bool  saveExprRows(const std::string& path);                            // 只重写 expr 行

// ── 自检 ──
std::string dump();   // MIYU_PARTS_DUMP=1 时启动打印：部位/带/参数/表情/跟随逐条
}
```

**为什么 header-only**：`partpoke.h:3-4` 那段注释说得很清楚 —— 这一轮不动
`CMakeLists.txt`，而两个 target 都把源文件列死（`CMakeLists.txt:27` 和 `:45`）。
header-only + `inline` 变量正好够用（`partpoke.h` 已经是这个套路）。
真要拆成 `.cpp`，就得同时改两个 target 的源文件列表（4 行），风险不大但要单独一步。

**`applyBands` 为什么不放进 `classify`**：带是**光栅化后处理**（每帧刷格子时应用一次），
不是点击时判定。现在的分工（渲染层刷格子、平台层查格子）是对的，别混。混了之后
`classify` 每点一次要重算 240×120 的带 → 点一下卡一帧。

### 4.4 加载失败的三层兜底

| 层 | 条件 | 行为 | 日志 |
|---|---|---|---|
| 1 | 文件不存在 / 读不到 | 空注册表；`classify` 恒空；点击走整套 plan（老路） | `[partapi] 读不到 parts.tsv，部位功能关闭（点击回整套反应）` |
| 2 | 单行语法错 | 跳过该行，继续读；行号进 `reg().bad` | `[partapi] parts.tsv:37 认不出记录类型 "bnd"，跳过` |
| 3 | 单条记录引用了未定义的部位 | 跳过并记 bad（**不自动建部位** —— 否则一个笔误长出第 15 个部位，格子 id 会漂） | `[partapi] parts.tsv:52 band 引用了未定义部位 "skirt2"，跳过` |

**取消 `kFallback` 那份内置常量表。** 它现在 9 条、文件 11 条，是今天「飘带判成 leg」
的埋点。兜底的正确做法是「降级到不用这张表也能跑」，不是「再抄一份可能过期的表」。

唯一的硬兜底留在编译期：`kZoneMax = 250`（`unsigned char` 上限内），超过就截断 + WARN。

---

## 五、迁移步骤（7 步 + 收尾，每步单独可验证、可回滚）

每一步的通用循环（硬约束照做）：

```bash
miyu-body-ctl stop
touch src/main.cpp          # 同一秒内改+编译会被 make 认为没变
cmake --build build -j4
miyu-body-ctl start
journalctl --user -u miyu-body -f -o cat   # 另开一格看日志
```

回滚单位是**一步**：代码 `git checkout -- <file>`（项目在 `~/.dsh/miyu` 下是 git 仓），
配置 `cp state/bak-parts-*/parts.tsv state/parts.tsv`。

### Step 0：备份 + 记基线（不改代码）

```bash
cd ~/.dsh/miyu/body/native3
mkdir -p state/bak-parts-$(date +%H%M) && cp state/{part-bands,param-parts,part-react}.tsv \
    state/bak-parts-$(date +%H%M)/
tools/part-poke-check.py                 # 12 部位 → state/poke-check.json（已有基线，重跑一次存档）
```

**验证**：`state/poke-check.json` 里 12 个部位都有数；日志里 `[zones] 判定带：… 11 条`、
`[web] 部位→表情：读进 12 条`、`读到 509 个参数的范围`。
**回滚**：无（只写了备份）。

### Step 1：`tools/parts-migrate.py`（三表 → parts.tsv + 反向自检）

它做两件事：
1. 读三张表 + 把 alias/part/follow 的元信息（从 `main.cpp:1062` 和 `legDragTick` 抄一遍，
   硬编码在脚本里，**只此一次**）生成 `state/parts.tsv`；
2. **反向生成**：解析刚写出来的 `parts.tsv`，再吐一份 `part-bands.tsv` / `param-parts.tsv` /
   `part-react.tsv` 的等价文本，和原件 `diff`（忽略注释与顺序，逐字段比）。

```bash
tools/parts-migrate.py --out state/parts.tsv
tools/parts-migrate.py --verify        # 反向 diff，不一致 exit 1
```

**验证**：`--verify` 三张表全 `identical`；`state/parts.tsv` 的数据行 = 13+13+13+13+12+3 = **67 行**（实测）。
**这一步不接入任何代码**，`parts.tsv` 只是躺在那里。回滚 = 删文件。

### Step 2：写 `src/partapi.h`，**不接入**，只让它自检

加 `MIYU_PARTS_DUMP=1` 的支持（放在 `main.cpp` 的 `web::init` 之后、Cubism 之前，
十行以内），启动时打印 `partapi::dump()`。

```bash
MIYU_PARTS_DUMP=1 ./build/miyu-wayland 2>&1 | head -80    # 一次前台跑，别动服务
tools/parts-dump.py --expect state/parts.tsv              # python 侧独立解析，对账
```

**验证**：两边输出逐条一致（部位 13 / band 13 / param 13 / expr 12 / follow 3，
每个部位的 id 和名字一致）。**回滚**：删掉这十行 + 那个头文件，服务不受影响。

### Step 3：band + alias 接入（最危险的一步，先做）

改三处：
- `main.cpp:1450-1515`：整块换成 `partapi::applyBands(...)`，删掉 `kFallback`；
  调用点保持在（alpha 白名单之后、泛洪之前）。
- `main.cpp:1062 zoneOfPartName`：换成 `partapi::indexOfByAlias(partName)`，删掉那 20 行 if。
- `partpoke.h:200-236 classify`：`scope=any` 走 band，删掉 `:207` 那段 ear 硬判。

**验证（三个都必须过）**：
1. 日志 `[zones] <部位>: u a..b v c..d` 十二行，逐行和 Step 0 的基线**对比**（这会暴露带边界闭/半开写错）；
2. `tools/part-poke-check.py` 12 部位倍数，与 `state/poke-check.json` 逐部位比（±10% 是量测抖动，
   超了就是改坏）；
3. 真点击（第二支指针）：
   ```bash
   python3 ~/.dsh/miyu/pointer/miyu_pointer.py --goto 433 224 --click   # 头
   ```
   日志要出 `[tap] … → 部位 head`。全部十一个点（全局坐标 = 窗口内 + ox 186 / oy 74）：

   | 部位 | 窗口内 | 全局 |
   |---|---|---|
   | 头 | 247,150 | 433,224 |
   | 头饰 | 215,140 | 401,214 |
   | 兽耳 | 180,205 | 366,279 |
   | 眼 | 247,193 | 433,267 |
   | 眉 | 252,174 | 438,248 |
   | 嘴 | 252,230 | 438,304 |
   | 胸 | 247,294 | 433,368 |
   | 腰 | 235,414 | 421,488 |
   | 右飘带 | 370,535 | 556,609 |
   | 左飘带 | 175,535 | 361,609 |
   | 腿 | 240,650 | 426,724 |

**回滚**：三个文件 `git checkout`，`state/part-bands.tsv` 一行没动（新表在 parts.tsv）。

### Step 4：param 接入

- `partpoke.h:239 hasParams` / `:246 top` → 转调 `partapi::hasAction` / `paramsOf`；
- 删掉 `:168-186` 那段 param-parts 解析；
- `main.cpp:1699 upOnly` → 读 param 行的 `up` 标记（`ch.upOnly`，由 API 填）。

**验证**：点一次，日志 `[part] 点中部位 chest -> 1 个根输入：PokeChest(+1.00)` 逐字和改前一致
（尤其 `(+1.00)` / `(-1.00)` 的交替——`gGen & 1` 的方向保序）；`part-poke-check.py` 全过。
**回滚**：同 Step 3。

### Step 5：expr 接入（热生效）

- `partpoke.h`/`main.cpp:1826 tapExprFor` → `partapi::pickExpr(part, seed, &name, &hold)`；
- `main.cpp:1864` 的 `5.0` → `hold`；
- `main.cpp:1855-1876 partPokeTick` 顶部的「有 expr 才算有反应」判定 → `hasAction`（只在这里用一次）；
- `platform_wayland.cpp:365-369` 的 `hasExpr` 拼装 → `partapi::hasAction(part)`，**删掉那 6 行**；
- `web.cpp`：`loadPartReact` 改成读 parts.tsv 的 expr 段（启动一次）；
  `savePartReact` 改成 `partapi::saveExprRows()`（行级保留 + 原子 rename + 写前重解析校验）；
  `/state` 多下发 `parts: [[name,label],…]`；前端 `PARTS` 用下发的列表（`web.cpp:343` 那 12 行删掉）。

`saveExprRows` 的实现要点（这是全套唯一有写入风险的路径，写清楚）：

```
读原文件全部行 → 保留非 expr 行原文（连同注释、空行、行尾空白）
              → 生成新的 expr 行（按注册表的部位顺序）
              → 写 .tmp → 重新解析 .tmp 校验（部位数、id 序列、band/param/follow 条数都没变）
              → 校验通过 rename，不通过删 .tmp 并日志报错
```

**验证**：
1. 设置页改「胸 → 兔牙」→ 点胸 → 立刻出兔牙、5 秒后回默认（**不重启**）；
2. `git diff state/parts.tsv` 只有 expr 段那几行变了（band/param/follow/注释一字未动）；
3. 故意把某行 band 写坏 → 写回被拒，日志有错、文件没变。

**回滚**：恢复旧版 `web.cpp`/`main.cpp` + `state/part-react.tsv`（parts.tsv 的 expr 段作废）。

### Step 6：follow 接入（拖动跟随通用化）

- 删掉 `main.cpp:3138-3192 legDragTick` 的硬编码，换 `partapi::followTick(...)`；
- 让位规则改成通用：`gPartPoke.on && gPartPoke.part == 该部位名` → 该部位的 follow 全跳过；
- 顺手修 `legSway` 不落盘：`saveSettings()` 补一个字段、`loadSettings()` 补一个分支。

**验证**：拖动她走一段，日志 `[follow] drag … HipAngleX=+… HipAngleZ=+… PokeLeg=+…`
三个数和改前 `[legdrag]` 那行**量级一致**（原值在日志里能翻到）；再临时给 `head` 加一条
`follow head ParamAngleZ 0.3 10 az`，拖动时头会晃 —— 这条是「新部位只改配置」的活证明。
**回滚**：`git checkout src/main.cpp` + `miyu-body-ctl restart`。

### Step 7：收尾（拆掉硬编码的旧址）

- `partpoke.h:58-61` `kZoneNames/kZoneCount` 删掉，全部换 `nameOf()/count()`；
  `main.cpp` 里 13 处 `partpoke::kZoneNames[...]` 跟着换（`:1207/1307/1314/1320/1338-1341/1377/1431/1489/1591-1594/1600/1606/1613/1643`，机械替换）；
- `state/{part-bands,param-parts,part-react}.tsv` 归档到 `state/legacy/`（留着当历史，不参与运行）；
- `state/*.bak-*` 清掉旧的（`bak-0211/1217/1338` 是 physics 备份，不是这套，别删）；
- `NOTES.md` 补一节「二十六、部位 API」（把这份文档结论抄过去，`NOTES.md` 是主账本）。

**验证**：`grep -rn "kZoneNames\|part-reacts.tsv" src/` 应该只剩注释；全量 `part-poke-check.py`
+ 11 点真点击 + 设置页一轮。
**回滚**：`git checkout src/ state/`（这一轮全部改动都在这些路径下）。

---

## 六、怎么验证「没改坏」

| 手段 | 位置 | 判据 |
|---|---|---|
| 倍数账本 | `tools/part-poke-check.py` → `state/poke-check.json` | 12 部位 own/other 倍数与基线逐部位比，±10% 内（量测抖动），且都 > 2（24.7 节里 eye/mouth/brow 本来就是 1.0/1.0/0.18，这三块的基线值是它们的**现状值**，不比 2） |
| 真点击 | 第二支指针 `~/.dsh/miyu/pointer/miyu_pointer.py --goto X Y --click` | 日志部位名与 4.2 的坐标表一致（11 个点） |
| 分区几何 | 日志 `[zones] <部位>: u .. v ..` | 与 Step 0 基线逐行一致（这是带边界写错的第一道报警） |
| 表一致性 | `tools/parts-migrate.py --verify` / `tools/parts-dump.py --expect` | 双向 identical |
| 热生效 | 设置页改一个表情 | 不重启、立刻生效、5 秒回默认 |
| 拖动 | 拖她走，看日志 `[follow]` | leg 三个量与旧 `[legdrag]` 同量级；新加部位也会晃 |
| 降级 | 临时 `mv state/parts.tsv /tmp/`，重启 | 点击仍能点、走整套 plan、日志一条 WARN，不崩 |
| 不回归 | 穿透计数 | 日志 `input region: … 条矩形 / … px / …%` 与改前一致（881 / 127912 / 28.4%） |
| 编译 | `cmake --build build -j4` | 两个 target **0 警告 0 错误**（`-Wall -Wextra`） |

---

## 七、留给审的人三个问题（可以否掉我的方案）

1. **`other` 要不要留在表里**（id 7）？留着能保证 id 1..13 与现状逐位对齐，代价是表里有个
   「什么都不能做」的部位。我的建议：留（对齐比好看重要）。
2. **web 写回整份 parts.tsv** 还是**保留 `part-react.tsv` 当覆盖层**（方案 B 的做法）？
   我选重写整份，因为它保证「一张表」这个性质；如果你觉得写回风险不值得，改成覆盖层
   只需要在 API 里加一层优先级，代码更少、风险更低、但「一张表」不彻底。
3. **alias 里的 `腰` 那条重复**（`chest` 那条也含 `腰`）要不要去重？建议**不去**（原样搬，
   行为逐位不变），去重是另一个话题。

---

## 八、这一轮没做的事（明确排除）

- 没有改任何 `.cpp` / `.h` / `CMakeLists.txt`；没有 `stop`/`start` 服务；没有跑
  `part-poke-check.py`（会启动带 `MIYU_POKE` 的实例，和正在跑的服务抢 layer surface）。
- 没有碰 `~/.dsh/plugins/`、`draginject`、`body/model/`、`body/voice/`。
- 没有重新量坐标（4.2/Step 3 里的 11 个点用的是你给的那份，全局坐标由 `state/window.json`
  的 ox=186 / oy=74 换算，窗口 500×900）。

---

## 九、阶段一实测（Step 0–2，2026-11-10 21:5x–22:1x）

**没碰 `web.cpp`**（ljm 当时在重做设置界面），也没碰 `build/`（他自己刚编过）。
Step 2 的自检走**独立二进制**：`g++ … tools/parts-selftest.cc` → `/tmp/parts-selftest`，
一条命令 1 秒，不起窗口、不建 layer surface、不动正在跑的服务。

### 9.1 Step 0 备份与基线

```
state/bak-parts-2152/{part-bands,param-parts,part-react}.tsv   ← 原件
state/bak-parts-2152/sha256.txt
state/bak-parts-2152/baseline-journal.txt    ← 最后一次启动的 1003 行日志
state/bak-parts-2152/baseline-geometry.txt    ← 12 个部位的 u/v 范围（改前）
```

工具侧基线（启动日志原文，改前 vs 改后要比的就是这几行）：

```
[zones] 判定带：从 state/part-bands.tsv 读了 11 条
[web] 部位→表情：读进 12 条（state/part-react.tsv）
[partpoke] 读到 509 个参数的范围
[zones] drawable 715 个，挂了 part 的 712 个，认得出部位的 484 个（head:113 chest:77 arm:49 skirt:56 leg:42 tail:11 eye:91 mouth:19 brow:4 ear:12 waist:6 headwear:4）
[plat-wl] input region: 881 条矩形 | 127912 px (28.4%) | bbox=[109,107 .. 446,837]
```

**⚠ 倍数基线缺一份**：`state/poke-check.json` 里只剩 head 一条（20:52 某次单点测试把
12 部位的结果覆盖掉了），不能当基线。所以 **Step 3 开工前必须补跑一次**
`tools/part-poke-check.py`（它会独占 layer surface，得先 `miyu-body-ctl stop`），
存成 `state/poke-check-baseline.json`。24.7 节那张表可以当「大概范围」参考，但那是
旧网格 + 旧参数的数，不能逐位比。

### 9.2 Step 1 `tools/parts-migrate.py`（已跑通）

```
$ ./tools/parts-migrate.py --migrate --verify
[ok] id 顺序：13 个部位与 src/partpoke.h kZoneNames[1..13] 逐位一致
[ok] 写出 state/parts.tsv（67 行数据）
[ok] state/part-bands.tsv       反向一致（11 行）
[ok] state/param-parts.tsv      反向一致（13 行）
[ok] state/part-react.tsv       反向一致（12 行）
[ok] 反向 diff 全部通过
```

三处实现细节是踩过才知道的（脚本里都有注释）：

1. **id 顺序是读源码比的**，不是脚本自己说了算：`kzone_names()` 正则抓
   `src/partpoke.h` 的 `kZoneNames` 字面量，和脚本里的 `PARTS` 逐位比，对不上就 exit 1。
   这样「id 不能重排」这条规矩由机器守，不靠人记。
2. **说明列是第 7/6 列，不是行尾注释** —— 旧表里「说明」是 TAB 分隔的字段
   （`部位 u0 u1 v0 v1 强制 说明`），第一次写成读行尾注释，diff 出来全是空。
3. **param 反向要按文件原始顺序摊**，不能按 score —— 运行时才按 score 排，
   表里的顺序是给人看的，两个混了 diff 一定红。

### 9.3 Step 2 `src/partapi.h` + 双端对账（已跑通）

```
$ /tmp/parts-selftest --dump  > /tmp/dump.cpp.txt
$ python3 tools/parts-dump.py > /tmp/dump.py.txt
$ diff /tmp/dump.py.txt /tmp/dump.cpp.txt
（无输出：115 行逐字节一致）
```

- C++ 侧 `stable_sort` 的稳定性和 python `list.sort` 的稳定性必须对得上 —— band 按
  `prio` 降序、param 按 `score` 降序，**同分保留文件顺序**，两边一致才 diff 得出来。
- 浮点格式定死：u/v `%.3f`、amp `%.3f`、gain `%.4f`、clamp `%.1f`、hold `%.1f`。

自检 39 条结构断言全 `[ok]`（注册表 / 表情 / alias 11 例 / 带进格子 / 11 个真点）：

```
  [ok] 部位数 13 · id(head)=1 · id(headwear)=13 · id(other)=7
  [ok] ear 4 条带（2 any + 2 body）· chest 0 条带 · head 0 个根参数（二十五拿掉的）
  [ok] mouth → ＞＜（hold 5.0）· arm 不放表情 · head → 前倾
  [ok] 女仆发饰A→13 · 眼镜框→1 · 左眼球→8 · 腰部衣服→12 · 短裙→4 · 组12→0
  [ok] applyBands 应用 11 条 · 格(59,51)=eye · fill 带不抢已有标签
  [ok] 11 个验证点全部判对（head/headwear/ear/eye/brow/mouth/chest/waist/skirt×2/leg）
```

**11 个点是拿文件里的 Z 行判的**，不是内存格子 —— 结果和真机日志里的判定完全一致，
说明「带 + 别名 + id」这三样翻译没走样。

### 9.4 三层兜底（已实测）

| 场景 | 结果 |
|---|---|
| 文件不存在（`MIYU_PARTS_FILE=/tmp/nope.tsv`） | `parts=0`，一条 WARN，exit 1，不崩 |
| 一行写错类型 + 两行引用未定义部位 | 好部位照读（2 个），坏行 3 条**逐条报行号和原文**，exit 0 |
| 手改用空格分隔（没有 TAB） | 报「这一行没有 TAB（是不是用空格分隔了？）」—— 这是手改表最常见的错 |

### 9.5 「加一个新部位只改配置」已经证明

不改 C++、不重编译（同一个 `/tmp/parts-selftest` 二进制），只在表尾追加四行：

```
part	toe	脚趾	3.0
band	toe	0.460	0.520	0.760	0.800	force	100
param	toe	PokeLeg	70	0.500
expr	toe	＞＜
```

```
$ MIYU_PARTS_FILE=/tmp/parts-toe.tsv /tmp/parts-selftest --grid 0.490 0.780
  指定点 u0.490 v0.780 → toe
$ … --dump | grep '^PART 14'
PART 14 toe 脚趾 hold=3.0 flags=-
```

这就是整个重构要的那一条性质，剩下的是把现有读点接过来（Step 3 往后）。

### 9.6 下一步（等 ljm 说 web.cpp 弄完）

Step 3 之前的准备工作只有一件：**补跑 `part-poke-check.py` 存基线**（见 9.1 的 ⚠）。

### 9.7 headwear 判定带：fill → force（Step 3 落地后立刻修的第一处配置）

2026-11-10 22:0x。11 点真点击（第二支指针）里**头饰那一点偶发判成 head**：同一坐标
`(215,140)` 两次点击给出两个结果。复测五个点，带内中间三点对、上下两点判 head。

根因不是 Step 3（28800 格逐格一致已证），是**配置和文档从 24.2 那天起就不一致**：

- `NOTES.md` 24.2 原话：「头饰也改成位置带：`v .118..0.172、u .395..0.610` 内**一律算头饰**」
- 但 `part-bands.tsv` 那两条写的是 `0`（补白 / fill），注释还写着「补白，不抢头发」

发饰是画在**头发上面**的白色蕾丝边，点它应该永远是发饰；fill 让呼吸说了算 —— 头发
drawable 压下来的那几帧，格子已经被 head 占掉，带子补不进去。

改法（只动配置，不动代码）：

```
band	headwear	0.395	0.470	0.118	0.178	force	100	# 女仆发饰左（force：发饰在头发上面）
band	headwear	0.535	0.610	0.118	0.178	force	100	# 女仆发饰右
```

`state/part-bands.tsv`（归档那份）同步改成 `1`，`tools/parts-migrate.py --verify` 仍然全绿。

**验证（第二支指针，连点三次同一坐标 `(215,140)` 全局 `(897,226)`，间隔 1.15s）**

```
[tap] 第 3 次点击 local=(215,140) → 部位 headwear（u=0.430 v=0.156，只动这一块）
[tap] 第 4 次点击 local=(215,140) → 部位 headwear（u=0.430 v=0.156，只动这一块）
[tap] 第 5 次点击 local=(215,140) → 部位 headwear（u=0.430 v=0.156，只动这一块）
（服务重启后又点一次：第 1 次 → headwear）
```
三次全对。改前是三次里一次 head、一次 headwear（第 2 / 第 12 次）。

**倍数（`tools/part-poke-check.py head headwear`）**

| 部位 | 改前 own / 倍数 | 改后 own / 倍数 |
|---|---|---|
| head | 5 px / 0.05 | 5 px / 0.07 |
| headwear | 686 px / 3.52 | **854 px / 6.14** |

headwear 涨是因为抢回了被头发占掉的那几块格子（自己 686→854，别的最大从 head 195 掉到 skirt 139）。
**head 几乎没变**（5 px 是「没有可拨的根」，只有表情），说明这两小块本来也不归它。
`state/poke-check-baseline.json` 里这两条已按新值更新，其余 10 个部位未动。

**教训**：注释会骗人。「补白，不抢头发」这句注释和设计意图（一律算头饰）是反的，
而 `NOTES.md` 24.2 记的是设计意图。两份数据打架的时候，**信行为**（同一坐标两次不同结果）
和**信原话**（一律），别信注释。

### 9.8 Step 5 前三步：分发按 id、表情从表里读、saveExprRows 单独验过

2026-11-10 22:1x–22:2x。`web.cpp` **一行没动**（等 ljm 的设置页回执）。

**① `partapi::classifyId()` 成为判定入口**

`classify()`（返回名字）改成 `classifyId()` 的壳。判定链没变（any 带 → 格子图 → Z 行），
只是返回 id 而不是拼字符串。

**② `platform_wayland.cpp` 的分发删掉现场拼装**

```cpp
const int partId = partapi::classifyId(u, v);
static int gLastPartId = 0;                       // 冷却按 id，不按名字
const bool samePart = (partId == gLastPartId) && (now - gLastPartT < TAP_SAME_PART_S);
if (partId > 0 && partapi::hasAction(partId) && !samePart) { ... }
```
删掉的三行是今天最后一个「两处各有一份数据」的现场：`partpoke::hasParams(part) ||
web::settings().partExpr[part]` —— 一半数据来自 parts.tsv、一半来自 part-react.tsv。

**③ `main.cpp` 的 `tapExprFor` 改从 partapi 读**

```cpp
if (!partapi::pickExpr(partapi::indexOf(part), seed, &pick, &hold)) return nullptr;
gTapExprHold = hold;
```
停留时间也跟着从表里来（`gTapExprUntil = now + gTapExprHold`，原来是写死的 `5.0`）。
expr 行没写 hold 就落 part 行的 5.0，行为不变。

**④ 表情覆盖层 + `saveExprRows`（唯一有写入风险的路径）**

语义定死：覆盖层是**同一份数据的未落盘版本**，不是第二份数据。

```
state/parts.tsv 的 expr 段   ← 唯一真源（重启后的事实）
        ↓ 启动读一次
partapi::reg().exprs          ← 只读
        ↑ 覆盖（只在网页改过的部位上有值，生命周期 = 进程）
```

`saveExprRows()` 的四条规矩（少一条就会吃掉别人手写的配置）：

1. 非 expr 行**逐字节原样保留**（注释、空行、行尾空白、`\r` 都算）
2. 新 expr 行写回**原位置**（原来按什么顺序排就还什么顺序；新部位追加到末尾）
3. 先写 `.tmp`，再**逐行 diff 断言**：两边去掉 expr 行后必须完全相等；读回来再断言一次
4. 断言不过就删 tmp、原文件一个字节不动、返回 false

**踩到的两个坑**

- **凭空造空行**：第一版把「注册表里有、原文件没有 expr 行」的部位也写出来，
  于是 `other` 每次多一行 `expr other `。改成「原来没有、现在也没值 → 不写」，幂等才成立
- **清空删不掉**：第一版 `setExprOverride(id, "")` 是 `erase` 覆盖层，结果落回注册表里
  文件原有的值 —— 网页上把下拉选回「无」，文件里那行不变。改成**空串也存**（显式清空）

**单独测（不接网页，在副本上）**

```
cp state/parts.tsv /tmp/parts-test.tsv
MIYU_PARTS_FILE=/tmp/parts-test.tsv /tmp/parts-selftest --save-expr /tmp/parts-test.tsv chest 兔牙
```

| 用例 | 结果 |
|---|---|
| 写回原值（幂等） | `cmp` **逐字节完全相同**；`parts-migrate.py --verify` 全绿（band/param/part-react 三表反向一致） |
| 改成「兔牙」 | `diff` 只差一行（`expr chest 脸红` → `兔牙`） |
| 清空 | 变成 `expr\tchest\t`（行还在，值为空） |
| 文件不存在 | `saveExprRows 失败：读不到原文件`，不崩 |
| 非 expr 行 | 98 行去掉 expr 后 sha256 与原件一致（`c9e36458…`） |

**服务侧验证（第二支指针，12 部位全覆盖 + 胸 ×3）**

```
第 1 次 (247,150) → head（id=1）          → 表情「前倾」
第 2 次 (215,140) → headwear（id=13）     → 表情「流泪」
第 3 次 (180,205) → ear（id=11）          → PokeEarL(-1.00) PokeEarR(-1.00)  表情「＞＜」
第 4 次 (247,193) → eye（id=8）           → PokeEye(+1.00)   表情「爱心眼」
第 5 次 (252,174) → brow（id=10）         → PokeBrow(+1.00)  表情「黑脸」
第 6 次 (252,230) → mouth（id=9）         → PokeMouth(+1.00) 表情「＞＜」
第 7 次 (247,294) → chest（id=2）         → PokeChest(-1.00) 表情「脸红」
第 8 次 (235,414) → waist（id=12）        → PokeWaist(+1.00) 表情「圈圈眼」
第 9 次 (370,535) → skirt（id=4）         → PokeSkirt(-1.00)（无表情，空行）
第 10 次 (175,535) → skirt（id=4）        → PokeSkirt(+1.00)
第 11 次 (240,650) → leg（id=5）          → PokeLeg(-1.00)（无表情）
第 12 次 (395,620) → tail（id=6）         → PokeTail(+1.00)（无表情）
第 13/14/15 次胸 → 表情「脸红」×3
第 16 次 (147,376) → arm（id=3）          → PokeArmL(+1.00) PokeArmR(+1.00)
```

12 个部位全中，参数名与前两轮一致，胸三次表情都在。
`认得出部位的 484 个`、`input region 881 / 127912 px / 28.4%` 都没变。

**还没做的**：`web.cpp` 三处（`loadPartReact` / `savePartReact` / `handleSet` 的 pexpr 分支）。
在那之前，`part-react.tsv` 仍会被 web 加载成 `web::settings().partExpr` —— 但**已经没有任何
代码读那个 map** 了，所以它现在是「死加载 + 死数据」，等 web.cpp 改完才清干净。
