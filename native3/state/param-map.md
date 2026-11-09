# 参数 → 部位 映射表（tools/param-probe.py 自动生成，别手改）

每个参数设成 min / max 各截一张，只数**她本人剪影内**的像素差；
部位是按她 bbox 归一化切的 9 块（tail/armL/armR/legL/legR/head/chest/waist/skirt）。
噪声底 = 0 px（静止两帧自己动的像素），低于 200 px 的算死参数。

## 活的参数（162 个，按总变化从小到大排）

| 参数 | 中文名 | 总像素 | 主要影响 | 分块明细 | 方向 |
| --- | --- | ---: | --- | --- | --- |
| `Clothes2` | 2. 大小变 | 121794 | **tail** | tail:27740 armL:10522 armR:7555 leg:16602 head:13043 chest:14573 skirt:22764 | max 侧更明显 |
| `ParamBodyAngleRX` | 身体位移RX | 105921 | **tail** | tail:21006 armL:9826 armR:8002 leg:11719 head:14945 chest:11580 skirt:19834 | max 侧更明显 |
| `ParamBodyAngleX2` | □胸腔　X | 103611 | **tail** | tail:22523 armL:9276 armR:7470 leg:10291 head:13174 chest:11134 skirt:19729 | min 侧更明显 |
| `ParamBodyAngleZ` | 胸腔　Z | 97167 | **tail** | tail:23329 armL:9553 armR:8103 leg:2087 head:14191 chest:11305 skirt:18447 | max 侧更明显 |
| `ParamBodyAngleY` | 胸腔　Y | 94293 | **skirt** | tail:17463 armL:9426 armR:6954 leg:7099 head:14747 chest:12563 skirt:18903 | min 侧更明显 |
| `ParamHipAngleZ` | □ 盆骨  Z | 66604 | **tail** | tail:24602 armL:3004 armR:2143 leg:13962 chest:1656 skirt:15880 | max 侧更明显 |
| `Clothes1` | 1. 长短裙切换 | 62139 | **skirt** | tail:16353 armL:2847 armR:2078 leg:16541 chest:2104 skirt:18067 | min 侧更明显 |
| `Param54` | Lean Y 3 | 61702 | **skirt** | tail:10356 armL:6831 armR:5324 leg:560 head:12074 chest:9056 skirt:12936 | max 侧更明显 |
| `ParamBodyAngleX` | 胸腔　X | 49250 | **tail** | tail:20429 armL:5221 armR:2159 leg:1920 head:580 chest:7995 skirt:4394 | min 侧更明显 |
| `HipAngleX2` | □盆骨  X | 46558 | **tail** | tail:18975 armL:3694 armR:1384 leg:1223 chest:3259 skirt:13328 | min 侧更明显 |
| `Param50` | Lean Y | 37756 | **head** | tail:28 armL:6006 armR:5850 leg:30 head:13010 chest:8234 skirt:691 | max 侧更明显 |
| `HipAngleX` | 盆骨X | 36203 | **skirt** | tail:3008 armL:3281 armR:1675 leg:9180 chest:2037 skirt:16653 | min 侧更明显 |
| `Clothes5` | 5. 尾巴 | 30870 | **tail** | tail:22762 leg:1504 skirt:1732 | min 侧更明显 |
| `SkirtY7` | 裙Y2 | 27861 | **skirt** | tail:399 armL:3142 armR:3278 leg:644 chest:2030 skirt:17681 | max 侧更明显 |
| `SkirtY6` | 裙Y | 27016 | **skirt** | tail:234 armL:3547 armR:2947 leg:409 chest:2405 skirt:16928 | max 侧更明显 |
| `ParamAngleX` | AngleX | 24627 | **head** | tail:17 armL:1700 armR:4207 leg:2 head:13679 chest:1552 skirt:32 | min 侧更明显 |
| `ParamAngleZ` | AngleZ | 23199 | **head** | tail:10 armL:1695 armR:3664 head:13413 chest:1492 skirt:21 | max 侧更明显 |
| `SkirtX1` | 裙X1 | 22341 | **skirt** | tail:74 armL:3185 armR:2721 leg:20 chest:1701 skirt:14201 | max 侧更明显 |
| `ParamAngleY` | AngleY | 21325 | **head** | tail:10 armL:1416 armR:3696 head:12313 chest:859 skirt:46 | min 侧更明显 |
| `SkirtSpecial2` | 裙X特殊2 | 20842 | **skirt** | tail:44 armL:2970 armR:1346 leg:28 chest:1788 skirt:14378 | max 侧更明显 |
| `SkirtY9` | 裙Y3 | 20674 | **skirt** | tail:226 armL:1743 armR:2306 leg:494 chest:378 skirt:15425 | max 侧更明显 |
| `LEGAngleZ2` | 双腿 Z | 20485 | **leg** | tail:3648 leg:13868 skirt:2179 | min 侧更明显 |
| `SkirtX2` | 裙X2 | 19221 | **skirt** | tail:78 armL:2025 armR:2425 leg:21 chest:832 skirt:13631 | max 侧更明显 |
| `Param52` | 尾巴翻面 | 17525 | **tail** | tail:12657 leg:988 skirt:1303 | min 侧更明显 |
| `Param_Angle_Rotation85` | [1]尾巴 | 16928 | **tail** | tail:10518 armR:362 leg:1679 skirt:1117 | min 侧更明显 |
| `Param53` | Lean Y 2 | 14731 | **chest** | armL:3391 armR:6 head:1110 chest:10128 skirt:96 | max 侧更明显 |
| `SkirtX3` | 裙X3 | 14338 | **skirt** | tail:21 armL:1199 armR:1545 leg:17 chest:199 skirt:11141 | max 侧更明显 |
| `Param_Angle_Rotation84` | [0]尾巴 | 14173 | **tail** | tail:9546 leg:1550 skirt:274 | min 侧更明显 |
| `Param33` | 并腿 | 14136 | **leg** | tail:2714 armL:2 leg:9863 skirt:1501 | min 侧更明显 |
| `LateHairZ` | 后发 Z | 13620 | **armR** | tail:15 armL:1831 armR:4169 leg:2 head:2831 chest:1354 skirt:29 | min 侧更明显 |
| `SkirtLayerY4` | 裙子层级Y4 | 12544 | **skirt** | tail:219 armL:175 armR:1036 leg:3 chest:16 skirt:10859 | max 侧更明显 |
| `Param_Angle_Rotation86` | [2]尾巴 | 11877 | **tail** | tail:7370 leg:852 skirt:875 | max 侧更明显 |
| `key10` | 10. 大钢盆 | 11704 | **head** | armL:13 head:11652 chest:7 skirt:1 | max 侧更明显 |
| `LateHairX1` | 后发X1 | 11098 | **armR** | tail:14 armL:1317 armR:4022 head:1924 chest:448 skirt:29 | min 侧更明显 |
| `LateHairY1` | 后发Y1 | 11021 | **armR** | tail:27 armL:1318 armR:4100 head:1850 chest:438 skirt:7 | max 侧更明显 |
| `Param_Angle_Rotation88` | [4]尾巴 | 10616 | **tail** | tail:10045 leg:156 skirt:1 | min 侧更明显 |
| `LateHairY2` | 后发Y2 | 10021 | **armR** | tail:8 armL:1250 armR:3977 leg:2 head:1118 chest:415 skirt:6 | max 侧更明显 |
| `LateHairX2` | 后发X2 | 9824 | **armR** | tail:14 armL:1195 armR:3734 head:1399 chest:395 skirt:25 | min 侧更明显 |
| `LateHairY3` | 后发Y3 | 8797 | **armR** | tail:24 armL:995 armR:3797 head:480 chest:310 skirt:5 | max 侧更明显 |
| `SkirtLayerX4` | 裙子层级X4 | 8766 | **skirt** | tail:11 armL:52 armR:692 leg:4 chest:11 skirt:7759 | max 侧更明显 |
| `LateHairX3` | 后发X3 | 8591 | **armR** | tail:17 armL:1131 armR:3664 head:382 chest:387 skirt:26 | min 侧更明显 |
| `ShoulderY` | 肩膀Y | 7404 | **armL** | tail:20 armL:2496 armR:1536 leg:9 head:414 chest:1746 skirt:623 | max 侧更明显 |
| `ShoulderZ` | 肩膀Z | 7278 | **armL** | tail:3 armL:2484 armR:1534 leg:9 head:412 chest:1711 skirt:623 | min 侧更明显 |
| `SkirtLayerY1` | 裙子层级Y1 | 7169 | **skirt** | tail:4 armL:1921 armR:6 leg:13 chest:1607 skirt:3618 | max 侧更明显 |
| `SideHairX1` | 侧发X1 | 7070 | **head** | tail:17 armL:418 armR:2 head:5660 chest:921 skirt:2 | min 侧更明显 |
| `SkirtLayerX1` | 裙子层级X1 | 6918 | **skirt** | tail:2 armL:2216 armR:75 leg:23 chest:1363 skirt:3239 | min 侧更明显 |
| `SkirtLayerX2` | 裙子层级X2 | 6475 | **skirt** | tail:1 armL:1994 armR:8 leg:19 chest:1237 skirt:3216 | min 侧更明显 |
| `SideHairY2` | 侧发Y2 | 6375 | **head** | tail:17 armL:452 armR:3 head:4939 chest:915 skirt:1 | max 侧更明显 |
| `LateHairX4` | 后发X4 | 6334 | **armR** | tail:26 armL:325 armR:3016 chest:132 skirt:32 | min 侧更明显 |
| `Param_Angle_Rotation87` | [3]尾巴 | 6272 | **tail** | tail:2677 leg:1652 skirt:747 | max 侧更明显 |
| `SideHairY1` | 侧发Y1 | 6222 | **head** | armL:470 head:4795 chest:955 skirt:2 | max 侧更明显 |
| `LateHairY4` | 后发Y4 | 5990 | **armR** | tail:13 armL:229 armR:2950 chest:41 skirt:31 | max 侧更明显 |
| `SideHairX2` | 侧发X2 | 5777 | **head** | armL:475 armR:3 head:4401 chest:896 skirt:2 | max 侧更明显 |
| `SkirtLayerY2` | 裙子层级Y2 | 5154 | **skirt** | tail:2 armL:1119 armR:4 leg:5 chest:405 skirt:3619 | max 侧更明显 |
| `key1` | 1. 黑脸 | 4964 | **head** | armL:1 head:4959 chest:3 skirt:1 | max 侧更明显 |
| `LaceRX` | < 花边X1 | 4946 | **skirt** | tail:12 armL:409 leg:526 head:52 chest:926 skirt:2426 | min 侧更明显 |
| `SideHairY3` | 侧发Y3 | 4634 | **head** | tail:9 armL:438 armR:1 head:3109 chest:1039 skirt:2 | max 侧更明显 |
| `LaceRX2` | < 花边X2 | 4555 | **skirt** | tail:12 armL:313 leg:448 head:4 chest:783 skirt:2461 | min 侧更明显 |
| `LArm` | 左大臂 | 4493 | **armR** | armR:2717 head:1 chest:1771 skirt:4 | max 侧更明显 |
| `RArm` | 右大臂 | 4483 | **armL** | tail:1 armL:2588 leg:5 chest:4 skirt:38 | max 侧更明显 |
| `LateHairX5` | 后发X5 | 4316 | **armR** | tail:12 armL:81 armR:2122 chest:14 skirt:25 | max 侧更明显 |
| `SideHairX3` | 侧发X3 | 4152 | **head** | armL:413 armR:1 head:2853 chest:884 skirt:1 | min 侧更明显 |
| `Clothes3` | 3. 胸口衣服 | 4146 | **chest** | tail:365 armL:724 armR:186 leg:68 head:254 chest:1809 skirt:156 | max 侧更明显 |
| `Param59` | 大臂透视 | 4057 | **armL** | tail:2 armL:2276 chest:2 skirt:562 | max 侧更明显 |
| `Param39` | 大臂透视 | 4023 | **armR** | armR:2596 leg:5 chest:1265 skirt:157 | max 侧更明显 |
| `LaceLX1` | > 花边X1 | 3875 | **chest** | tail:690 leg:474 head:63 chest:1314 skirt:1292 | min 侧更明显 |
| `LaceLX2` | > 花边LX2 | 3521 | **skirt** | tail:600 leg:400 head:4 chest:1095 skirt:1422 | max 侧更明显 |
| `SkirtLayerY5` | 裙子层级Y5 | 3515 | **skirt** | tail:31 armR:100 skirt:3162 | max 侧更明显 |
| `Movement1` | 1. 狗尾草 | 3407 | **armR** | armR:2218 chest:263 skirt:926 | max 侧更明显 |
| `LaceRX3` | < 花边X3 | 3291 | **skirt** | tail:12 armL:198 leg:352 head:3 chest:592 skirt:1552 | min 侧更明显 |
| `ChestY1` | 胸Y1 | 3209 | **chest** | armL:923 armR:1 leg:28 head:1 chest:2256 | max 侧更明显 |
| `ChestX2` | 胸X2 | 3003 | **chest** | tail:9 armL:803 leg:46 head:6 chest:2138 skirt:1 | min 侧更明显 |
| `LaceLX3` | > 花边LX3 | 2962 | **skirt** | tail:879 leg:316 chest:798 skirt:969 | min 侧更明显 |
| `SkirtLayerX5` | 裙子层级X5 | 2960 | **skirt** | tail:91 armR:96 skirt:2628 | min 侧更明显 |
| `FrontHairY1` | 前发Y1 | 2842 | **head** | armL:9 head:2829 chest:3 skirt:1 | max 侧更明显 |
| `ChestX1` | 胸X1 | 2814 | **chest** | tail:9 armL:782 leg:23 head:6 chest:1994 | min 侧更明显 |
| `FrontHairX1` | 前发X1 | 2807 | **head** | tail:17 armL:1 head:2727 chest:3 skirt:1 | min 侧更明显 |
| `ChestY2` | 胸Y2 | 2803 | **chest** | tail:89 armL:746 leg:17 head:4 chest:1946 skirt:1 | max 侧更明显 |
| `ChestY4` | 胸↓ | 2700 | **chest** | armL:769 leg:23 head:4 chest:1903 skirt:1 | min 侧更明显 |
| `ChestX3` | 胸X3 | 2599 | **chest** | tail:4 armL:813 leg:18 head:2 chest:1762 | min 侧更明显 |
| `SkirtLayerY3` | 裙子层级Y3 | 2571 | **skirt** | tail:1 armL:784 armR:3 leg:7 chest:330 skirt:1446 | max 侧更明显 |
| `Movement2` | 2. 抚胸 | 2548 | **skirt** | armL:820 skirt:1151 | max 侧更明显 |
| `RForearm` | 右小臂 | 2526 | **armL** | tail:2 armL:971 skirt:523 | max 侧更明显 |
| `Movement3` | 3. 竖食指 | 2498 | **armR** | armR:2179 chest:200 skirt:119 | max 侧更明显 |
| `LForearm` | 左小臂 | 2460 | **armR** | tail:4 armL:67 armR:1859 chest:451 skirt:46 | max 侧更明显 |
| `FrontHairY2` | 前发Y2 | 2432 | **head** | armL:1 head:2426 chest:3 skirt:2 | max 侧更明显 |
| `ChestXBS` | 胸XBS | 2404 | **chest** | tail:115 armL:715 leg:28 head:1 chest:1495 | min 侧更明显 |
| `SkirtLayerX3` | 裙子层级X3 | 2342 | **skirt** | tail:110 armL:777 armR:4 leg:13 chest:378 skirt:1018 | max 侧更明显 |
| `FrontHairX2` | 前发X2 | 2247 | **head** | tail:17 armL:2 head:2166 chest:2 | max 侧更明显 |
| `ChestY3` | 胸Y3 | 2195 | **chest** | tail:3 armL:744 leg:25 head:1 chest:1422 | max 侧更明显 |
| `Param41` | R小臂 Z | 2195 | **armR** | tail:86 armR:1577 head:93 chest:438 skirt:1 | min 侧更明显 |
| `Param60` | L小臂 Z | 2156 | **armL** | tail:33 armL:888 leg:7 chest:2 skirt:2 | min 侧更明显 |
| `AngleYbs` | 角度 Ybs | 1697 | **tail** | tail:389 armL:73 armR:169 leg:112 head:258 chest:24 skirt:138 | max 侧更明显 |
| `SideHairY4` | 侧发Y4 | 1639 | **head** | armL:258 head:969 chest:412 | max 侧更明显 |
| `Param36` | R大臂z | 1636 | **chest** | tail:119 armL:1 armR:174 head:479 chest:811 skirt:2 | max 侧更明显 |
| `SideHairX4` | 侧发X4 | 1509 | **head** | armL:269 armR:1 head:807 chest:429 skirt:3 | max 侧更明显 |
| `SideHairRBS` | 侧发RBS | 1502 | **head** | tail:207 armL:52 armR:212 leg:95 head:224 chest:11 skirt:122 | max 侧更明显 |
| `FrontHairX3` | 前发X3 | 1486 | **head** | armL:3 head:1480 chest:1 skirt:2 | max 侧更明显 |
| `LRibbonX4` | 左飘带X4 | 1456 | **armR** | tail:199 armL:41 armR:248 leg:175 head:179 chest:34 skirt:41 | min 侧更明显 |
| `LaceY1` | 缎带Y1 | 1439 | **armL** | tail:7 armL:661 armR:2 head:249 chest:519 skirt:1 | max 侧更明显 |
| `FrontHairY3` | 前发Y3 | 1430 | **head** | tail:15 armL:4 head:1360 | max 侧更明显 |
| `Param34` | 大小变透明参数 | 1426 | **chest** | tail:214 armL:338 armR:1 leg:49 head:341 chest:441 | max 侧更明显 |
| `Hat_XA` | 帽子 XA | 1412 | **head** | tail:86 armL:1 head:1320 chest:5 | min 侧更明显 |
| `LaceY2` | 缎带Y2 | 1249 | **armL** | tail:45 armL:523 head:228 chest:403 skirt:1 | max 侧更明显 |
| `Hat_YA` | 帽子 YA | 1200 | **head** | armL:2 head:1197 chest:1 | min 侧更明显 |
| `LaceX3` | 缎带X3 | 1078 | **armL** | armL:515 armR:2 head:140 chest:420 skirt:1 | max 侧更明显 |
| `LaceX1` | 缎带X1 | 1029 | **armL** | tail:7 armL:465 armR:1 head:186 chest:370 | min 侧更明显 |
| `LaceY3` | 缎带Y3 | 1014 | **armL** | tail:10 armL:456 armR:2 head:194 chest:352 | max 侧更明显 |
| `Hat_XB` | 帽子 XB | 966 | **head** | armL:1 head:964 chest:1 | max 侧更明显 |
| `key7` | 7. 脸红 | 934 | **head** | head:933 chest:1 | max 侧更明显 |
| `LaceX2` | 缎带X2 | 918 | **armL** | tail:6 armL:421 armR:2 head:135 chest:353 skirt:1 | min 侧更明显 |
| `LControl` | < Control | 899 | **head** | armL:8 head:891 | min 侧更明显 |
| `RControl` | > Control | 896 | **head** | head:894 chest:2 | min 侧更明显 |
| `LEar1` | <兽耳PHA | 857 | **head** | armL:7 head:849 skirt:1 | min 侧更明显 |
| `REar1` | >兽耳PHA | 855 | **head** | head:853 chest:1 skirt:1 | min 侧更明显 |
| `Hat_YC` | 帽子 YC | 825 | **head** | tail:39 armL:1 head:735 chest:1 | max 侧更明显 |
| `key3` | 3. 圈圈眼 | 814 | **head** | tail:128 head:636 chest:1 skirt:2 | max 侧更明显 |
| `Param62` | L 大臂z | 805 | **armL** | armL:635 head:166 skirt:2 | max 侧更明显 |
| `Clothes4` | 4. 鞋子 | 781 | **tail** | tail:781 | max 侧更明显 |
| `Hat_YB` | 帽子 YB | 766 | **head** | armL:2 head:761 chest:3 | max 侧更明显 |
| `key12` | 12. 单片眼镜 | 750 | **head** | tail:124 head:571 chest:1 | max 侧更明显 |
| `key8` | 8. 加载 | 743 | **head** | head:742 chest:1 | max 侧更明显 |
| `key11` | 11. 黑框眼镜 | 715 | **head** | tail:94 armL:2 head:618 chest:1 | max 侧更明显 |
| `key6` | 6. 屑屑眼 | 687 | **head** | head:684 chest:1 skirt:2 | max 侧更明显 |
| `RSleeveY1` | 右袖子Y1 | 679 | **armL** | armL:520 chest:1 skirt:49 | max 侧更明显 |
| `RSleeveX1` | 右袖子X1 | 675 | **armL** | armL:514 chest:1 skirt:78 | max 侧更明显 |
| `LSleeveX1` | 左袖子X1 | 669 | **armR** | armR:392 chest:277 | max 侧更明显 |
| `LSleeveY` | 左袖子Y1 | 668 | **armR** | armR:449 chest:217 skirt:2 | max 侧更明显 |
| `CheeksWiggleY` | 脸盘抖动Y | 659 | **head** | head:655 chest:4 | min 侧更明显 |
| `LEar2` | <兽耳PHB | 643 | **head** | armL:9 head:634 | max 侧更明显 |
| `REar2` | >兽耳PHB | 636 | **head** | head:634 chest:1 skirt:1 | max 侧更明显 |
| `key5` | 5. > <眼 | 616 | **head** | tail:16 head:599 chest:1 | max 侧更明显 |
| `RSleeveY2` | 右袖子Y2 | 612 | **armL** | armL:446 chest:1 skirt:60 | max 侧更明显 |
| `Hat_XC` | 帽子 XC | 601 | **head** | head:601 | max 侧更明显 |
| `LSleeveY2` | 左袖子Y2 | 595 | **armR** | armR:384 chest:210 skirt:1 | max 侧更明显 |
| `LSleeveX2` | 左袖子X2 | 578 | **armR** | armR:313 chest:236 skirt:1 | min 侧更明显 |
| `RSleeveX2` | 右袖子X2 | 548 | **armL** | armL:423 chest:1 skirt:60 | min 侧更明显 |
| `RFlip2` | < Flip y | 548 | **head** | armL:9 head:538 skirt:1 | min 侧更明显 |
| `LFlip2` | > Flip y | 527 | **head** | head:527 | min 侧更明显 |
| `FrontHairY4` | 前发Y4 | 491 | **head** | armL:2 head:488 chest:1 | min 侧更明显 |
| `FrontHairX4` | 前发X4 | 473 | **head** | armL:2 head:469 chest:2 | max 侧更明显 |
| `LFlip` | > Flip x | 458 | **head** | tail:86 head:372 | min 侧更明显 |
| `LSleeveX3` | 左袖子X3 | 450 | **armR** | armR:269 chest:170 skirt:1 | min 侧更明显 |
| `AngleXbs` | 角度 Xbs | 442 | **tail** | tail:289 leg:68 | max 侧更明显 |
| `RSleeveY3` | 右袖子Y3 | 435 | **armL** | armL:280 chest:1 skirt:64 | max 侧更明显 |
| `ParamEyeBallX` | 眼珠 X | 430 | **head** | tail:17 head:361 chest:1 skirt:1 | max 侧更明显 |
| `ParamEyeLOpen` | 左眼　开闭 | 426 | **head** | tail:15 head:359 skirt:1 | min 侧更明显 |
| `LSleeveY3` | 左袖子Y3 | 425 | **armR** | armR:329 chest:96 | max 侧更明显 |
| `RSleeveX3` | 右袖子X3 | 418 | **armL** | armL:294 chest:1 skirt:55 | min 侧更明显 |
| `RFlip` | < Flip x | 394 | **head** | armL:9 head:385 | max 侧更明显 |
| `LEar3` | <兽耳PHC | 342 | **head** | tail:107 armL:4 head:191 | max 侧更明显 |
| `ParamEyeROpen` | 右眼  开闭 | 322 | **head** | head:320 chest:2 | min 侧更明显 |
| `ParamEyeBallY` | 眼珠 Y | 304 | **head** | head:304 | min 侧更明显 |
| `ShortPonytailY` | 短马尾Y | 304 | **tail** | tail:227 | max 侧更明显 |
| `key4` | 4. 爱心眼 | 298 | **head** | tail:44 head:204 | max 侧更明显 |
| `LEyes1` | 眼眶1 | 239 | **head** | tail:17 head:173 | min 侧更明显 |
| `LHand` | 左手指 | 226 | **skirt** | armR:81 skirt:145 | min 侧更明显 |
| `RHand` | 右手指 | 225 | **skirt** | skirt:43 | min 侧更明显 |
| `REyes1` | 眼眶1 | 218 | **head** | tail:17 head:150 | max 侧更明显 |
| `key2` | 2. 落泪 | 212 | **head** | head:211 chest:1 | max 侧更明显 |
| `Param_Angle_Rotation89` | [5]尾巴 | 210 | **tail** | tail:210 | min 侧更明显 |
| `EyeSmile` | 微笑 | 201 | **head** | tail:14 head:183 | min 侧更明显 |

## 死参数（347 个，设成 min/max 画面几乎不动）

`AngleX`、`AngleXbs2`、`AngleXbs3`、`AngleY`、`AngleYbs2`、`AngleZ`、
`Angry`、`Angry2`、`Angry3`、`AngryMove`、`AngryMove2`、`AngryMove3`、
`AngryMove4`、`AngryMove5`、`AngryMove6`、`AngryMove7`、`AngryMove8`、`Ani1`、
`Ani2`、`AniEyerolling`、`BrowInnerUp`、`CheeckPuff`、`CheeckPuff2`、`CheeckPuffL`、
`CheeckPuffR`、`CheeksWiggleX`、`CheeksWiggleXL`、`CheeksWiggleXR`、`Colorflicker5`、`Colorflicker6`、
`Colorflicker7`、`Colorflicker8`、`Divider1`、`Divider10`、`Divider11`、`Divider12`、
`Divider13`、`Divider14`、`Divider15`、`Divider16`、`Divider18`、`Divider19`、
`Divider20`、`Divider22`、`Divider23`、`Divider24`、`Divider25`、`Divider28`、
`Divider3`、`Divider31`、`Divider33`、`Divider4`、`Divider5`、`Divider6`、
`Divider7`、`Divider8`、`Divider9`、`Emotion`、`Emotion2`、`F1Anikey`、
`F2Anikey`、`F3Anikey`、`FeatherPhysics1`、`FeatherPhysics2`、`FeatherPhysics3`、`FeatherPhysics4`、
`FrontHairZ`、`GemHighlightx`、`GemHighlighty`、`GemReflection`、`Glintshift5`、`Glintshift6`、
`Glintshift7`、`Glintshift8`、`Hat2phB`、`Hat2phC`、`Hat2phD`、`Hat2pha`、
`HipAngleXq`、`HipAngleY`、`HipAngleYDOWN`、`HipAngleYUP`、`HipAngleZ`、`HipZ1`、
`HipZ2`、`IDLE`、`JawOpen`、`KiraA`、`KiraB`、`KiraC`、
`KiraD`、`LBowX1`、`LBowX3`、`LBowX4`、`LCry`、`LCry3`、
`LCryKey`、`LEyeLashX1`、`LEyeLashX2`、`LEyeSquint`、`LEyeball1`、`LEyeball2`、
`LEyeball3`、`LEyes2`、`LHand2`、`LHighLight1`、`LHighLight2`、`LHighLight3`、
`LJewelry1`、`LJewelry2`、`LJewelry3`、`LParamGlowA`、`LParamGlowB`、`LParamGlowC`、
`LRibbonX1`、`LRibbonX2`、`LRibbonX3`、`LRibbonY1`、`LRibbonY2`、`LRibbonY3`、
`LRibbonY4`、`LWrist`、`LateHairY5`、`LateSideHairX1`、`LateSideHairX2`、`LateSideHairX3`、
`LateSideHairX4`、`LateSideHairX5`、`LateSideHairXBS`、`LateSideHairY1`、`LateSideHairY2`、`LateSideHairY3`、
`LateSideHairY4`、`LateSideHairY5`、`LateSideHairYbs`、`LeanInput`、`LipPhysics1`、`LipPhysics2`、
`LipPhysics3`、`LittleFlower1`、`LittleFlower10`、`LittleFlower11`、`LittleFlower12`、`LittleFlower13`、
`LittleFlower2`、`LittleFlower3`、`LittleFlower4`、`LittleFlower5`、`LittleFlower6`、`LittleFlower9`、
`LowX2`、`MicroGlows`、`MouthFunnel`、`MouthPressLipOpen`、`MouthPuckerWiden`、`MouthRollLower`、
`MouthRollUpper`、`MouthSmile_L`、`MouthSmile_R`、`MouthX`、`Mouthpucker`、`NoseNeer`、
`Param`、`Param11`、`Param12`、`Param13`、`Param132`、`Param133`、
`Param135`、`Param136`、`Param137`、`Param138`、`Param139`、`Param140`、
`Param141`、`Param142`、`Param2`、`Param25`、`Param27`、`Param28`、
`Param29`、`Param30`、`Param31`、`Param32`、`Param35`、`Param37`、
`Param38`、`Param40`、`Param42`、`Param43`、`Param44`、`Param45`、
`Param46`、`Param47`、`Param48`、`Param49`、`Param51`、`Param55`、
`Param56`、`Param57`、`Param58`、`Param6`、`Param61`、`Param7`、
`ParamA`、`ParamBodyAngleRY`、`ParamBodyAngleRZ`、`ParamBodyAngleY2`、`ParamBreath`、`ParamBrowLAngle`、
`ParamBrowLAngleSmoothing`、`ParamBrowLY`、`ParamBrowLY2`、`ParamBrowLY3`、`ParamBrowRAngle`、`ParamBrowRAngleSmoothing`、
`ParamBrowRY`、`ParamBrowRY2`、`ParamBrowRY3`、`ParamE`、`ParamHipAngleZ2`、`ParamI`、
`ParamMouthForm`、`ParamMouthOpenY`、`ParamO`、`ParamSilence`、`ParamTongueUpDown`、`ParamTongueWidth`、
`ParamU`、`Paramshuiyin7`、`Paramshuiyin8`、`PositionX`、`PositionY`、`PositionZ`、
`PoutOffsetXL`、`PoutOffsetXR`、`Press`、`PupilX`、`PupilY`、`RBowX1`、
`RBowX2`、`RBowX3`、`RBowX4`、`RBowX5`、`RBowX6`、`RBowX7`、
`RBowX8`、`RColorshift`、`RCry`、`RCry2`、`RCryKey`、`REar3`、
`REyeLashX1`、`REyeLashX2`、`REyeSquint`、`REyeball1`、`REyeball2`、`REyeball3`、
`REyes2`、`RHand2`、`RHighLight1`、`RHighLight2`、`RHighLight3`、`RJewelry1`、
`RJewelry2`、`RJewelry3`、`RParamGlowA`、`RParamGlowB`、`RParamGlowC`、`RRibbonX1`、
`RRibbonX2`、`RRibbonX3`、`RRibbonX4`、`RRibbonY1`、`RRibbonY2`、`RRibbonY3`、
`RRibbonY4`、`RShiftbuffer`、`RSleeveX4`、`RWrist`、`SHOCKEYE`、`SWITCHA`、
`Shine`、`ShineBIG`、`ShineX`、`ShockL`、`ShockR`、`ShortPonytailX1`、
`ShortPonytailX2`、`ShortPonytailXBS`、`ShortPonytailXBS2`、`ShortPonytailY2`、`ShortPonytailYBS`、`ShortPonytailYBS2`、
`ShoulderX`、`ShoulderYy`、`SideHairLBS`、`SideHairLX1`、`SideHairLX2`、`SideHairLX3`、
`SideHairRX1`、`SideHairRX2`、`SideHairRX3`、`SkirtLayerX6`、`SkirtLayerY6`、`SkirtSpecial`、
`SkirtX4`、`SkirtY`、`SkirtY2`、`SkirtY3`、`SkirtY4`、`SkirtY5`、
`SkirtY8`、`SkirtYBS`、`Startwinkle`、`Startwinkle2`、`Sweat1`、`Sweat2`、
`SweatBurst1`、`SweatBurst2`、`SweatBurst3`、`SweatBurst4`、`SweatBurst5`、`SweatBurst6`、
`TongueOut`、`WingOpenClose`、`WingOpenClose2`、`WingRotation`、`WingRotation2`、`WingSpread`、
`WingSpread2`、`key13`、`key9`、`ribbonX1`、`ribbonX2`、`ribbonX3`、
`ribbonX4`、`ribbonX5`、`ribbonX6`、`ribbonX7`、`ribbonY1`、`ribbonY2`、
`ribbonY3`、`ribbonY4`、`ribbonY5`、`ribbonY6`、`ribbonY7`