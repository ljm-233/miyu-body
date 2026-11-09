# 物理图（tools/physics-map.py 生成）

源：`/home/link/.dsh/miyu/body/model/大肥鱼.physics3.json`

| 项 | 数 |
|---|---|
| PhysicsSetting | 131 |
| 参数节点 | 323 |
| 边（输入→输出） | 773 |
| 根（只被输入，从不被输出） | 31 |
| 叶子（只被输出，从不被输入） | 273 |
| 中间节点（两头都占） | 19 |
| 反馈环（强连通分量 >1） | 0 |
| 最长传导链 | 5 层 |

## 根：可以拨的（31）

| 参数 | 显示名 | 部位 | 驱动几条 setting |
|---|---|---|---|
| `AngleX` | OX | other | 8 |
| `AngleY` | OY | other | 12 |
| `AngleZ` | OZ | other | 7 |
| `BrowInnerUp` | BrowInnerUp | head | 2 |
| `CheeckPuff` | 鼓脸 | head | 3 |
| `Clothes1` | 1. 长短裙切换 | skirt | 3 |
| `Clothes2` | 2. 大小变 | head | 6 |
| `EyeSmile` | 微笑 | head | 4 |
| `JawOpen` | 下巴开闭 | head | 1 |
| `LEyeSquint` | L-Eye Squint | head | 1 |
| `LeanInput` | Lean Input | other | 19 |
| `MouthX` | MouthX | head | 1 |
| `Mouthpucker` | 撅嘴 | head | 1 |
| `Movement1` | 1. 狗尾草 | tail | 1 |
| `Movement2` | 2. 抚胸 | chest | 1 |
| `Movement3` | 3. 竖食指 | head | 1 |
| `Param42` | R手腕 Z | arm | 1 |
| `ParamBreath` | 呼吸 | other | 30 |
| `ParamBrowLY` | 左眉上下 | head | 7 |
| `ParamBrowRY` | 右眉上下 | head | 7 |
| `ParamEyeBallX` | 眼珠 X | head | 1 |
| `ParamEyeBallY` | 眼珠 Y | head | 3 |
| `ParamEyeLOpen` | 左眼　开闭 | head | 7 |
| `ParamEyeROpen` | 右眼  开闭 | head | 7 |
| `ParamMouthForm` | 嘴　变形 | head | 6 |
| `ParamMouthOpenY` | 嘴　张开和闭合 | head | 4 |
| `Paramshuiyin7` | -----下面是按键----- | head | 1 |
| `PositionX` | OPX | other | 8 |
| `PositionY` | OPY | other | 22 |
| `PositionZ` | OPYZ | other | 10 |
| `REyeSquint` | R-Eye Squint | head | 1 |

## 每个部位的根输入

「拨这几个，这块就会自己动起来」—— 从叶子反推祖先，只留根。

### head（叶子 125 个，根 26 个）

| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |
|---|---|---|---|
| `BrowInnerUp` | BrowInnerUp | 1 | 100% |
| `CheeckPuff` | 鼓脸 | 1 | 100% |
| `EyeSmile` | 微笑 | 1 | 100% |
| `JawOpen` | 下巴开闭 | 1 | 100% |
| `LEyeSquint` | L-Eye Squint | 1 | 100% |
| `MouthX` | MouthX | 1 | 100% |
| `Mouthpucker` | 撅嘴 | 1 | 100% |
| `ParamEyeBallX` | 眼珠 X | 1 | 100% |
| `ParamEyeBallY` | 眼珠 Y | 1 | 100% |
| `ParamEyeLOpen` | 左眼　开闭 | 1 | 100% |
| `ParamEyeROpen` | 右眼  开闭 | 1 | 100% |
| `ParamMouthForm` | 嘴　变形 | 1 | 100% |
| `ParamMouthOpenY` | 嘴　张开和闭合 | 1 | 100% |
| `Paramshuiyin7` | -----下面是按键----- | 1 | 100% |
| `REyeSquint` | R-Eye Squint | 1 | 100% |
| `ParamBrowLY` | 左眉上下 | 1 | 83% |
| `ParamBrowRY` | 右眉上下 | 1 | 83% |
| `AngleZ` | OZ | 1 | 70% |
| `AngleX` | OX | 1 | 68% |
| `AngleY` | OY | 1 | 68% |
| `Clothes2` | 2. 大小变 | 1 | 67% |
| `ParamBreath` | 呼吸 | 1 | 40% |
| `LeanInput` | Lean Input | 1 | 27% |
| `PositionX` | OPX | 2 | 7% |
| `PositionZ` | OPYZ | 2 | 6% |
| `PositionY` | OPY | 2 | 3% |

### chest（叶子 10 个，根 5 个）

| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |
|---|---|---|---|
| `PositionZ` | OPYZ | 1 | 11% |
| `LeanInput` | Lean Input | 1 | 8% |
| `PositionY` | OPY | 1 | 7% |
| `ParamBreath` | 呼吸 | 1 | 3% |
| `PositionX` | OPX | 2 | 10% |

### arm（叶子 31 个，根 12 个）

| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |
|---|---|---|---|
| `Movement1` | 1. 狗尾草 | 1 | 100% |
| `Movement2` | 2. 抚胸 | 1 | 100% |
| `Movement3` | 3. 竖食指 | 1 | 100% |
| `PositionZ` | OPYZ | 1 | 24% |
| `PositionY` | OPY | 1 | 23% |
| `PositionX` | OPX | 1 | 23% |
| `ParamBreath` | 呼吸 | 1 | 15% |
| `LeanInput` | Lean Input | 1 | 6% |
| `ParamBrowLY` | 左眉上下 | 2 | 17% |
| `ParamBrowRY` | 右眉上下 | 2 | 17% |
| `AngleX` | OX | 2 | 2% |
| `AngleY` | OY | 2 | 2% |

### skirt（叶子 72 个，根 9 个）

| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |
|---|---|---|---|
| `Clothes1` | 1. 长短裙切换 | 1 | 71% |
| `PositionY` | OPY | 1 | 52% |
| `LeanInput` | Lean Input | 1 | 36% |
| `PositionZ` | OPYZ | 1 | 36% |
| `ParamBreath` | 呼吸 | 1 | 28% |
| `PositionX` | OPX | 2 | 35% |
| `AngleZ` | OZ | 2 | 24% |
| `AngleX` | OX | 2 | 23% |
| `AngleY` | OY | 2 | 23% |

### leg（叶子 15 个，根 7 个）

| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |
|---|---|---|---|
| `PositionX` | OPX | 1 | 11% |
| `PositionZ` | OPYZ | 1 | 9% |
| `PositionY` | OPY | 1 | 8% |
| `LeanInput` | Lean Input | 1 | 6% |
| `Clothes2` | 2. 大小变 | 1 | 4% |
| `AngleX` | OX | 1 | 1% |
| `AngleY` | OY | 1 | 1% |

### tail（叶子 20 个，根 11 个）

| 根参数 | 显示名 | 几跳摸到这块 | 影响落在这块的比例 |
|---|---|---|---|
| `Param42` | R手腕 Z | 1 | 100% |
| `Clothes2` | 2. 大小变 | 1 | 29% |
| `Clothes1` | 1. 长短裙切换 | 1 | 29% |
| `LeanInput` | Lean Input | 1 | 18% |
| `ParamBreath` | 呼吸 | 1 | 14% |
| `AngleZ` | OZ | 1 | 5% |
| `AngleX` | OX | 1 | 5% |
| `AngleY` | OY | 1 | 5% |
| `PositionZ` | OPYZ | 2 | 15% |
| `PositionX` | OPX | 2 | 14% |
| `PositionY` | OPY | 2 | 7% |

## 最长传导链（5 层）

`ParamBrowLY` → `ParamBrowLAngleSmoothing` → `ParamBrowLAngle` → `ShockL` → `LHighLight1`

