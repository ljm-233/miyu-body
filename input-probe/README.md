# input-probe — Wayland 桌宠「只有不透明像素吃点击」验证

环境：Arch + niri 26.04 + eDP-1 2560x1600 @ scale 1.5（逻辑 1706x1066，布局偏移 x=1920）

## 结论

`wl_surface.set_input_region()` 在 niri 26.04 上**生效**，协议层零错误，物理点击已实测通过。

## 文件

| 文件 | 作用 |
| --- | --- |
| `alpha_region.h/.c` | **可复用模块**：RGBA8888 像素缓冲 → `wl_region*`，按行扫描 + 竖直合并 |
| `test_region.c` | 模块纯逻辑单测（不连 Wayland），27 项检查全过 |
| `input-probe.c` | 最小探针：400x400 overlay 浮层 + 红色圆，input region 只覆盖圆；同进程再建一个 600x600 的 recorder 垫在下面当「下层窗口」 |
| `clicker.c` | 用 `zwlr_virtual_pointer_v1` 注入绝对移动 + 左键点击（不需要 ydotool / root） |
| `regression.sh` | 一键跑完整验证并清理 |
| `protocols/` `gen/` | wlr-layer-shell / wlr-virtual-pointer / xdg-shell 协议 XML 与 wayland-scanner 产物 |

## 编 & 跑

```bash
make                                    # 出 input-probe / clicker / test_region
./test_region                           # 单测，末行 PASS
./input-probe &                         # 探针（eDP-1 左上 +(160,160)，圆 R=150）
niri msg layers                         # 应看到 Namespace: "probe" / "recorder" 在 Overlay 层
./clicker 2280 360                      # 全局坐标：圆心 → 被探针吃掉
./clicker 2100 180                      # 全局坐标：四角 → 穿透到 recorder
pkill -x input-probe                    # 收工（SIGTERM 1ms 内退出）
```

全局坐标换算：`global = (1920 + 160 + local_x, 0 + 160 + local_y)`。

## alpha_region 接口

```c
/* 纯逻辑扫描：按行找 alpha>threshold 的连续段，竖直方向同 x/w 的相邻行合并成竖带 */
size_t alpha_region_scan(const uint8_t *pixels, int32_t width, int32_t height,
                         size_t stride, uint8_t threshold,
                         struct alpha_rect *rects, size_t max_rects);

/* 扫描 + 建 wl_region（空图返回空 region = 完全穿透），调用者负责 destroy */
struct wl_region *alpha_region_build(struct wl_compositor *comp,
                                     const uint8_t *pixels, int32_t width, int32_t height,
                                     size_t stride, uint8_t threshold, size_t max_rects);

/* build + set_input_region + destroy，一步到位 */
void alpha_region_apply(struct wl_surface *surface, struct wl_compositor *comp,
                        const uint8_t *pixels, int32_t width, int32_t height,
                        size_t stride, uint8_t threshold, size_t max_rects);
```

注意：坐标是 **surface-local 逻辑像素**。若 buffer 按 `buffer_scale=2` 渲染（HiDPI 清晰度），
要先降采样到逻辑尺寸再喂，否则 region 会被放大一倍。

## 实测结果（eDP-1，圆 R=150，圆心 local (200,200)）

| 全局坐标 | local | 期望 | 实际 |
| --- | --- | --- | --- |
| (2280,360) 圆心 | (200,200) | probe 吃掉 | `BUTTON probe (200,200)` ✅ |
| (2130,360) 圆内靠左 | (50,200) | probe 吃掉 | `BUTTON probe (50,200)` ✅ |
| (2100,540) 左下角 | 圆外 | 穿透 | `leave probe → enter recorder → BUTTON recorder` ✅ |
| (2460,180) 右上角 | 圆外 | 穿透 | `BUTTON recorder (480,120)` ✅ |
| (2280,190) 圆外 3px | (200,30) | 穿透 | `BUTTON recorder` ✅ |

region 统计：175 条矩形，覆盖 70672 px / 160000 px（圆 R=150 理论面积 π·150² ≈ 70686，误差 0.02%）。

## 踩过的坑

1. `zwlr_layer_shell_v1.get_popup` 引用 `xdg_popup_interface`，链接时必须带上 xdg-shell 的协议代码，否则 undefined reference。
2. `wl_display_dispatch` 是阻塞读，信号处理里置 flag 没用 —— 要用 `poll(fd, 200ms)` 包一层，否则 `pkill` 后窗口留在屏幕上。
3. niri 同层（overlay）内 surface 按创建顺序堆叠：**后创建的在上面**。记录器必须先建。
4. eDP-1 报 `wl_output.scale = 2`（整数），实际 niri 用 1.5 —— 逻辑坐标以 `niri msg outputs` 的 Logical size 为准，别用 wl_output 的 scale 算。
5. `zwlr_virtual_pointer_v1.frame` 不带时间参数（v2 签名），别照着 wlroots 老代码写。
