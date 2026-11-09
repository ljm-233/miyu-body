/* alpha_region.h — 按 alpha 生成 wl_surface input region
 *
 * 用途：Wayland 桌面浮层（桌宠、悬浮球）只有不透明像素接收鼠标，
 *       透明部分 click-through。唯一正确做法就是 wl_surface.set_input_region()。
 *
 * 两层 API：
 *   1) alpha_region_scan()  —— 纯逻辑，无 Wayland 依赖，可单测。
 *      按行扫描 RGBA8888，把每行 alpha>threshold 的连续段合成矩形带。
 *   2) alpha_region_build() —— 把扫描结果写进 wl_region。
 *
 * 坐标：均为 surface-local 逻辑像素，原点左上，y 向下（与 wl_region 一致）。
 *       像素缓冲的宽高等于 surface 的逻辑尺寸（即 buffer scale = 1）。
 *       若按 scale 渲染（如 scale=1.5 用 600x600 buffer 画 400x400 surface），
 *       请先把图降采样到逻辑尺寸，或自行把返回值除以 scale。
 */
#ifndef ALPHA_REGION_H
#define ALPHA_REGION_H

#include <stddef.h>
#include <stdint.h>
#include <wayland-client.h>

struct alpha_rect {
	int32_t x, y, w, h;
};

/* 扫描 alpha>threshold 的像素，按行合并成矩形带。
 *
 * pixels   : RGBA8888（小端即 ARGB8888，字节序 R,G,B,A），首行为顶部
 * width    : 逻辑宽（像素）
 * height   : 逻辑高（像素）
 * stride   : 每行字节数（通常 width*4，可为负的 0 表示紧凑排布）
 * threshold: alpha 阈值，alpha > threshold 才算不透明（默认建议 8）
 * rects    : 输出数组，可为 NULL 只计数
 * max_rects: 输出数组容量
 *
 * 返回：命中的矩形总数（可能大于 max_rects；写入数 = min(返回, max_rects)）。
 *       返回 0 表示整张图都透明 —— 此时应设一个空 region，窗口完全不可点。
 */
size_t alpha_region_scan(const uint8_t *pixels, int32_t width, int32_t height,
			 size_t stride, uint8_t threshold,
			 struct alpha_rect *rects, size_t max_rects);

/* 扫描并直接生成 wl_region*。调用者负责 wl_region_destroy()。
 * comp 为 NULL 或扫描结果为空时返回一个空 region（不是 NULL），
 * 空 region 语义 = 整个 surface 都不接收输入（完全穿透）。
 * max_rects 建议 4096；超出的部分丢弃并返回实际写入数（可用 scan 先算）。
 */
struct wl_region *alpha_region_build(struct wl_compositor *comp,
				    const uint8_t *pixels, int32_t width, int32_t height,
				    size_t stride, uint8_t threshold,
				    size_t max_rects);

/* 便捷：一次到位 —— build + set + destroy。 */
void alpha_region_apply(struct wl_surface *surface, struct wl_compositor *comp,
			const uint8_t *pixels, int32_t width, int32_t height,
			size_t stride, uint8_t threshold, size_t max_rects);

/* 矩形带覆盖的总面积（像素数），单测用来断言。 */
size_t alpha_region_area(const struct alpha_rect *rects, size_t n);

#endif /* ALPHA_REGION_H */
