# native2 —— 原生 Live2D 身体（X11 + GLFW + 桌面 GL + Cubism Native）

## 根因：贴图是不完整的 mipmap 纹理，桌面 GL 对它采样返回 (0,0,0,1)

黑剪影不是因为 `u_baseColor` 是 0，也不是 sampler 没绑上，是**纹理本身不完整**。

链路：

1. `Framework/src/Rendering/OpenGL/CubismShader_OpenGLES2.cpp` 的 `SetupTexture()` 每帧给模型贴图设参数：

   ```cpp
   glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
   ```

   （官方 Sample 这么写，是因为它自己的 `TextureManager` 上传后调了 `glGenerateMipmap`。）

2. `src/main.cpp` 的 `load_tex()` 只 `glTexImage2D` 了 level 0，**从来没生成 mip 链**。

3. MIN_FILTER 是 mipmap 过滤、又没有 mip 链 → 纹理 incomplete。桌面 GL 对不完整纹理的采样结果是
   **`(0,0,0,1)`**（不是 0 向量，是"黑且不透明"）。

4. 于是 `texColor = (0,0,0,1)` → `color.rgb = 0`、`color.a = 1` →
   `gl_FragColor = vec4(color.rgb*color.a, color.a) = (0,0,0,1)`。

5. 混合模式是用户自己设的 `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)`（premultiplied，正确）：
   `0 + dst*(1-1) = 0` → **纯黑、且把背后的壁纸完全遮住**，正好是"完整轮廓 + 全黑 + 窗口不透明处只剩剪影"。

这个解释同时满足所有已知现象：

- 几何对（顶点走的是 client array，跟纹理无关）
- GL 上下文/GLEW/上传/UV 全都正常（`test.cpp` 对照成立：它自己建纹理、没被 `SetupTexture` 改成 mipmap 过滤）
- 改成 `#version 120`、换 GPU、换 GL/GLES 都不变（mipmap 不完整跟驱动和版本无关）
- 诊断成"每个像素采同一个点"也还是黑（不完整纹理采哪都是 `(0,0,0,1)`）
- Wayland/EGL 那条路也不出颜色（同一个原因，不是 EGL 的锅）

## 改了什么

`src/main.cpp` 的 `load_tex()`，`glTexImage2D` 之后加一行：

```cpp
glGenerateMipmap(GL_TEXTURE_2D);   // 配上 SetupTexture 设的 GL_LINEAR_MIPMAP_LINEAR
```

`FrameworkShaders/FragShaderSrc.frag` 之前被打上的诊断行（`gl_FragColor = texture2D(s_texture0, vec2(0.5,0.5));`）
已还原成 `FragShaderSrc.frag.bak` 的原版，现在两者 `diff` 为空。

## 怎么验证的

```bash
cd build && make
ps -eo pid,comm | awk '$2=="miyu"{print $1}' | xargs -r kill
env -u WAYLAND_DISPLAY DISPLAY=:0 ./build/miyu &   # 必须 X11
sleep 5 && grim /tmp/check.png
```

窗口浮在屏幕右侧（物理坐标约 x=3870..4430, y=250..1210，niri 缩放 1.5），裁那块看：
截图 `shot-color.png` 在仓库根目录。

像素统计（改前/改后）：

| | 躯干区平均 RGB（crop 内坐标 300:600, 180:380） | 亮像素（>180 任一通道）占比 |
|---|---|---|
| 改前（黑剪影） | (6.5, 9.4, 13.8) | 0.421 |
| 改后 | (140.9, 147.3, 178.1) | 0.602 |

（亮像素占比改前不是 0，因为这块 crop 里还含窗口外的蓝色壁纸/背景，剪影本身是真黑。）

改后截图里蓝发、白围裙、肤色、尾巴、腿环的颜色都对，说明不仅是"采到了图"，
`u_baseColor` / `u_multiplyColor` / `u_screenColor` 三个 uniform 也都在正常值上（如果是 0，这里会整体发黑或全白）。

## 走过的死路（别再试）

- **Wayland layer-shell + EGL**：窗口能浮能透明，但纹理采样怎么都不出颜色。
  桌面 GL / GLES2 / GLES3 全试过，连自造 2×2 纯红图都采不出来 —— 那次的真正原因同样是
  `SetupTexture` 设的 mipmap 过滤 + 没 mip 链；换成 X11 后症状一模一样，就是这条线索。
- **GLFW 走 Wayland 后端**：普通窗口，niri 不给浮层（跟渲染无关，是窗口类型问题）。
- **怀疑 `u_baseColor` 被设成 0**：不成立。若 baseColor 为 0，alpha 也是 0，剪影会直接消失而不是变黑。
  变黑恰好说明 alpha 是 1、只有 rgb 是 0 —— 这是"采样拿到了 (0,0,0,1)"的指纹。
- **怀疑 sampler 没绑到纹理单元**：不成立，`SetupTexture` 里 `glActiveTexture(GL_TEXTURE0)` +
  `glUniform1i(SamplerTexture0Location, 0)` 都在，而且若 sampler 无效，采样会走 texture 0 的默认值，
  同样是 `(0,0,0,1)`，但那时 `glGetTexParameter` 之类的自检是能看出来的。
- **别把 `glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA)` 改成 `GL_SRC_ALPHA`**：Cubism 的输出是
  premultiplied alpha，改了就二次乘 alpha，只会更黑。

## 下一步（还没做）

- 现在每帧都调 `SetupTexture` 改一次纹理参数，等于每帧把 mipmap 过滤重新设一遍；
  真要省事可以改成在 `load_tex` 里就设 `GL_LINEAR_MIPMAP_LINEAR`，行为一致。
- 表情 / 动作 / 呼吸（`CubismMotionManager`、`CubismEyeBlink`、`CubismBreath`）还没接，现在只是静态站姿。
  （`physics3.json` 已在 2026-11-09 下午接上，见下面那节。）
- 窗口位置硬编码 `glfwSetWindowPos(win, 1900, 150)`，niri 缩放变了要跟着改。

---

# 2026-11-09 下午： 「人形散开」这一轮

## 结论先写

**清掉实验代码、干净 `make` 之后，她本来就是站好的**（`shot-stand.png`，480×885 窗口裁图）。
这一轮真正的产出不是"把散的拼回去"，而是把"散开"这类现象**钉死成参数问题**，顺手把 physics 接上了。

三条"病征"里两条不是病：

1. **背后那块蓝紫渐变 = 桌面壁纸透过透明窗口**，不是贴图被拉伸。
   验证方式：主循环里把 `renderer->DrawModel()` 整个跳过（当时的临时开关 `MIYU_NODRAW`），
   人不见了，但窗口里那片蓝紫**一模一样**。所以它跟模型、贴图、UV 一点关系都没有。
   窗口是 `GLFW_TRANSPARENT_FRAMEBUFFER` + `glClearColor(0,0,0,0)`，看到的本来就是后面的壁纸。
2. **"没有 pose" 不成立**：`大肥鱼.model3.json` 的 `FileReferences` 里只有 `Physics` 和 `DisplayInfo`，
   **没有 `Pose` 键**；`body/model/` 下也没有 `pose3.json`。`CubismPose` 对这套模型是空转，不用接。
   （运行日志里现在会打 `[main] pose = (model3.json 里没有 Pose)`。）

## "参数全 0" 也不成立，而且 LoadParameters() 本来就不背这个锅

`MIYU_DUMP=1` 打出来：**509 个参数，逐个 v == default**（498 个是 0，11 个从 1 起，比如 ParamEyeLOpen）。

原因在框架里：

```cpp
// CubismModel::Initialize()
_parameterValues = Core::csmGetParameterValues(_model);   // 直接指向 Core 内部数组
                                                          // Core 装载 moc3 时填的就是 defaultValues

// CubismModel::LoadParameters()
const csmInt32 savedParameterCount = _savedParameters.GetSize();   // 初始 0
if (parameterCount > savedParameterCount) parameterCount = savedParameterCount;  // → 0
for (...) _parameterValues[i] = _savedParameters[i];               // 循环跑 0 次
```

`_savedParameters` 一开始是空的，没人调 `SaveParameters()` 就一直是空的，
所以**第一次 `LoadParameters()` 是空操作**，参数保持 Core 装进来的默认值。
"第一次运行参数可能是全 0"这个担心可以划掉。

## 那"散开"到底是什么

**参数值不对**。证据（都做过）：

- 把每个参数推到 max 跑一次（临时开关 `MIYU_PARAMMAX`）：人立刻变成散件 ——
  头戴金属盆、躯干消失、手臂和腿各自飞出去、几个服装变体同时露脸。
  形状和 `fx2-c.png` 是**同一类**：躯干还在原地，四肢和尾巴因为挂在非恒等的变形器上而甩出去。
- 反过来把参数放回默认值，站姿就永远是对的（复现 5 次，含干净重编）。

**试过、排除掉的：**

- **跳过 `model->Update()`**：画面一模一样，一点没变。
  因为 moc3 里存的顶点本来就是"默认姿势下的最终位置"，默认参数下变形器是恒等变换。
  ⇒ "忘了 Update 导致部件散开"不成立。
- **贴图 / UV / 混合**：跳过 `DrawModel()` 蓝紫还在（见上）；颜色、发型、裙子的花纹都对。
- **变形器链 / 蒙版**：`[draw]` 里每个部件的 mask 数、tex 号、opacity 都在合理范围；
  并集 bbox `[-0.458,-0.930 .. 0.500,0.882]` = 0.957×1.812（画布 1.00×2.05），
  就是个占满画布、上窄下宽的人形，不是散的。

⇒ 看到"散开"不要再怀疑渲染管线，**先去打参数**（`MIYU_DUMP=1`）。默认值一放回去就站好了。

## 改了什么（这一轮只有一处行为改动）

1. **接 physics**（`src/main.cpp`）：头发、袖子、尾巴、裙子现在有物理演算了。
   ```cpp
   #include "Physics/CubismPhysics.hpp"
   // 建：
   physics = Csm::CubismPhysics::Create(json.data(), json.size());   // 读 setting.GetPhysicsFileName()
   // 每帧（顺序照官方 Sample：LoadParameters → physics → Update）：
   model->LoadParameters();
   if (physics) physics->Evaluate(model, dt);
   model->Update();
   ```
   `dt` 取 `glfwGetTime()` 差，**超过 0.1 s 就截断** —— 首帧或卡顿后的一个大步长会把物理甩飞。
   日志会打 `[main] physics = 大肥鱼.physics3.json (217421 字节) -> ok`。
   想关掉就 `physics = nullptr;`，一行。
2. **留了两个诊断开关**（不影响正常运行）：
   - `MIYU_DUMP=1` 打参数表（id/value/default/min/max）+ 每个 drawable 的
     `vc/op/tex/mask/vis/bbox`，末尾一行"可见 drawable 数 + 并集 bbox"。排查几何问题第一手材料。
   - 每进程打一次 `[win] pos=(x,y) size=WxH framebuffer=WxH`。
     **截图别再用记忆里的坐标**：niri 会自己挪窗口，今天上午的 (3870,250,4430,1210) 下午就失效了
     （这次窗口跑到物理 (4117,510)-(4597,1395)）。先 `grep "\[win\]"` 再裁。

## 怎么验证的

```bash
cd /home/link/.dsh/miyu/body/native2/build && make
ps -eo pid,comm | awk '$2=="miyu"{print $1}' | xargs -r kill
cd .. && env -u WAYLAND_DISPLAY DISPLAY=:0 ./build/miyu > /tmp/run.log 2>&1 &
sleep 6 && grim /tmp/check.png
grep "\[win\]" /tmp/run.log     # 拿真实窗口矩形，再拿 PIL 裁
```

数据：

| 项目 | 结果 |
|---|---|
| 参数值 vs 默认值 | 509 / 509 相等（不是全 0） |
| 可见 drawable | 460 / 715（其余 opacity=0，正常） |
| 全部可见部件并集 bbox | 0.957 × 1.812（画布 1.00 × 2.05，占满、人形） |
| 跑 2.5 分钟后 vs 开局 | 差异像素 9151 / 537600（≈1.7%，都是头发边缘/抗锯齿），人形不漂 |
| physics | `Create` 返回非空，`Evaluate` 每帧跑，静态站姿下画面不变（没有输入就没有摆动，正常） |
| 角度自检 | 把 `ParamAngleX/Z` 推到 25/12 跑一遍：整体跟着转，**没有散** |

## 还没做 / 已知小怪

- **眨眼**：`model3.json` 的 `Groups` 里已经写好 `EyeBlink`（`ParamEyeLOpen`/`ParamEyeROpen`），
  接 `CubismEyeBlink` 就有眨眼。没接，现在眼睛是睁着不动的。
- **呼吸 / 凝视**：`CubismBreath`、`CubismLook` 都没接。物理现在"看不出来"就是因为没有输入动作。
- **动作（motion3.json）**：没接。
- **`GetParameterId(i)->GetString()` 打出来是乱码**（`MIYU_DUMP` 里那列 id 不可读），
  但 physics / 内部按 id 查参数都正常工作，暂时只影响日志可读性。真要修再单独查。
- 每帧都调一次 `SetupTexture` 改贴图参数的浪费还在（上个节点记的），不影响画面。

---

# 2026-11-09 13:2x~13:3x：「修回站立」这一轮

## 结论先写

**她站着，和 `shot-stand.png` 一模一样**（`shot-stand-check.png`，另有一张左右对比 `shot-compare.png`）。
而且**散架复现不出来**：同一个 binary、同一份 json，我跑了十几遍（单进程 6 s / 13 s / 116 s / 200 s、
关 physics、开 physics、两个进程、强制播 IDLE），**没有一次散**。

真正查出来的东西是这一条：**散的那一层不是这个进程画的。**

## 硬证据（可复现，不是猜）

`/tmp/run5.log`、`run6.log`、`run7.log`（出散图那三次，13:18:30 / 13:19:13 / 13:19:52）
和我这次**站着**的 `run1.log`、`run2.log`，`diff` 出来是**空的**，五个文件全都 554 字节、逐字节相同：

```
[main] physics = 大肥鱼.physics3.json (217421 字节) -> ok
[main] pose = (model3.json 里没有 Pose)
[main] eyeBlink = ok
[main] 动作组 0 个
[win] pos=(1920,0) size=501x900 framebuffer=501x900
```

同一份代码、同一份配置、同一行日志，一张散一张站 ⇒ **散出来的那些部件不是这个进程画的**。
程序这边能写参数的路只剩 physics，而 physics 开/关我都试了，都站。

再加上另一条：`t3 / t4-c / t5-c / t6c / t7c` 五张散图**构图完全一样**（同一堆碎片、同一个位置），
而它们跨了四次重启。固定散件不会这样，**屏幕上多出来的一层**才会这样。

## 所以是怎么"散"的（推断，机制上只有这一条讲得通）

窗口是 `GLFW_TRANSPARENT_FRAMEBUFFER` + `glClearColor(0,0,0,0)`，**背景全透明**。
只要还活着的旧 miyu 进程没杀干净，它那扇窗就和新窗叠在几乎同一点
（两边都 `glfwSetWindowPos(win, 1900, 150)`，niri 报的 pos 一直是 `(1920,0)`，
但物理位置每次会漂几十像素 —— 这就是"各在各的位置"的来源）：

> 新窗口（站着的她）+ 旧窗口（散的那份，用的是 13:19:47 那版还带实验代码的 main.cpp）
> 透在一起 → 头、躯干、手臂、腿各在各的位置。

这条同时解释了最反直觉的那件事：**"关动作 / 关眨眼 / 关 physics，都还是散的"** ——
你改的是新起的那个进程，散的那层根本不吃这些开关。上一任交付时"清掉实验代码就好了"也是同理：
他清代码的时候顺手把旧进程一起清了。

我现在站得住，是因为我第一步就把所有 miyu 一次性杀干净并确认 `count=0`：

```bash
ps -eo pid,comm | awk '$2=="miyu"{print $1}' | xargs -r kill
ps -eo comm | grep -cx miyu      # 必须是 0
```

**这条是判断，不是铁证**（进程都死了，现场没了）。要一锤定音看下面「下次怎么 30 秒确诊」。
「我哪一步弄散的」这个问题，按证据只能这么回答：**没有哪一步把模型改散**，
那三处改动（Motions 引用 / 眨眼+动作管理器 / `MIYU_NO_PHYS`）没有一处会动几何 ——
真正让画面散的是**多开了一个旧进程且它一直在屏幕上**。

## 顺便纠正两条之前写错的

1. **「`IDLE.motion3.json` 直接播会把人播散」不成立。**
   我另编了一个强行播 IDLE 的二进制（`/tmp/idle/exp`，**没动仓库任何文件**，
   `StartMotionPriority(idleMotion,false,3)` + 每帧 `UpdateMotion(model,dt)`），
   跑出来是**完整站立、只是微微摆姿势**。
   会散的是 `Animation/` 里另外那些带服装/体型切换的（脱捕 / 裙子切换 / 体型），不是 IDLE。
2. **`大肥鱼.model3.json` 的 `Groups` 被改过，但不影响任何东西。**
   现在写的是 `{"Target":"EyeBlink","Name":"EyeBlink",...}`，而 `.bak`（01:39 原版）是
   `"Target":"Parameter"` 多一个空的 `LipSync` 组。
   框架只按 **Name** 匹配：`CubismModelSettingJson.cpp:155 IsExistEyeBlinkParameters()` 里只
   `strcmp(..., Name, "EyeBlink")`，`Target` 根本没读。所以眨眼照样能接，不用回滚。
   （按你说的，这个 json 我一个字节没动。）

## 改了什么（很小）

`src/main.cpp`（备份在 `/tmp/main.cpp.before-clean`）：

- **删掉三块死代码**：`CubismEyeBlink::Create(&setting)`、`Csm::CubismMotionManager`、
  读 `IDLE.motion3.json` 那一段，以及它们对应的 `#include`。
  它们**从来没被每帧调用过**（没有 `StartMotion`、没有 `UpdateMotion`、`eyeBlink->UpdateParameters` 是注释掉的），
  对画面零影响，只是启动时多读几个文件、多打几行会骗人的日志。
- **主循环拆掉空转的那一段** `if (motionMgr != nullptr) { ... }`，现在是：
  `model->LoadParameters()` → `physics->Evaluate(model, dt)` → `model->Update()` → `DrawModel()`，
  和上一任交付时的行为**完全一致**。
- **保留** `MIYU_DUMP=1`（只在环境变量存在时打印，无害）。
- **保留** `MIYU_NO_PHYS`（physics 那一行的守卫，无害且以后二分有用）。

新增 `run.sh`（18 行）：杀干净 → 确认 0 个 → 起一个 → 日志落 `/tmp/miyu-run.log`。
以后一律用它起，不再手敲。

## 怎么验证的

```bash
cd /home/link/.dsh/miyu/body/native2/build && make
cd .. && ./run.sh
sleep 6 && grim /tmp/final1.png
python3 - <<'PY'   # 定位窗口（找"白且旁边有饱和蓝"的像素，白字终端不会命中）
from PIL import Image; import numpy as np
a=np.asarray(Image.open('/tmp/final1.png').convert('RGB')).astype(int)
r,g,b=a[:,:,0],a[:,:,1],a[:,:,2]; m=(r>215)&(g>215)&(b>210)&((b-r)>45)
print(np.argwhere(m).min(0), np.argwhere(m).max(0))
PY
```

这次窗口在物理 `(3757,264)-(4262,1167)`，裁 `(3720,250)-(4300,1240)` 就是 `shot-stand-check.png`。

| 项目 | 结果 |
|---|---|
| 参数 vs 默认 | 509 个，498 个为 0、11 个从 1 起 —— 与上一任交付时**逐项相同** |
| 可见 drawable | 460 / 715 |
| 全部可见部件并集 bbox | 0.957 × 1.812（画布 1.00×2.05，人形占满） |
| 姿态 | 和 `shot-stand.png` 同姿势（头微侧、双手在身前、尾巴在右下） |
| 连续 13 s 取 6 帧（physics 开） | 6 帧都是完整站立 |
| t=6 s vs t=116 s | 差异 5.5% 像素，全是头发边缘抗锯齿，构图不变 |
| `MIYU_NO_PHYS=1` | 站立 |
| 强制播 `IDLE.motion3`（临时二进制） | 站立，只是微微摆姿势 |
| 两个进程同时跑 | 两扇窗同位置同相位，看不出区别（也说明"叠一层"确实会透） |

## 下次怎么 30 秒确诊（别再看图猜）

出现"散"的时候，**先数进程、数窗格，看一眼都别急着改代码**：

```bash
ps -eo pid,comm | awk '$2=="miyu"'          # 必须只有 1 行
niri msg windows | grep -c 'App ID: "native2-test"'   # 必须只有 1
```

不是 1 就是它。杀干净再起一次，散不散立刻见分晓。

## 想加眨眼 / 动作的正确做法

- **眨眼（安全，先做这个）**：`Groups` 里已经有 `ParamEyeLOpen` / `ParamEyeROpen`，
  `CubismEyeBlink::Create(&setting)` 能直接拿到（见上面第 2 条，`Target` 写错也不影响）。
  接法：建一次，主循环里 `LoadParameters()` **之后**、`Update()` **之前**调
  `eyeBlink->UpdateParameters(model, dt)`。就这一行，不会动任何变形器。
- **动作（危险，别直接喂 `Animation/` 那 8 个）**：那 8 个是给**另一版模型**录的，
  同一个参数名在另一版里正好是"服装切换 / 体型 / 脱捕"那一路，喂进这套 509 参数上就会
  把手臂、尾巴、裙子推到极端。正确做法：
  1. 自己做一个 motion3，**只**保留安全曲线：`ParamAngleX/Y/Z`、`ParamEyeBallX/Y`、
     `ParamBodyAngleX`、`ParamBreath`、`ParamMouthForm`。别的曲线一条都别留。
  2. 接法：`motionMgr->StartMotionPriority(motion, false, 3)` 一次，主循环
     `LoadParameters()` 之后调 `motionMgr->UpdateMotion(model, dt)`。
  3. 第一次播之前存一份 main.cpp，播完 `MIYU_DUMP=1` 对一遍参数（509/509 == 默认才算安全）。
- **physics 现在"看不出来"是正常的**：没有输入（`ParamAngleX` / `ParamBodyAngleX` / `ParamBreath`
  都是默认值）就没有摆动。要么等有了动作/凝视驱动，要么手动给 `ParamBodyAngleX` 一个正弦，
  头发和尾巴就会动起来。
