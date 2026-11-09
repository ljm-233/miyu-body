#define _GNU_SOURCE
#include "alpha_region.h"

#include <stdlib.h>
#include <string.h>

/* 一行里的一段，带竖直累积信息（x/w 相同的相邻行会合并成一条竖带） */
struct seg {
	int32_t x, w;
	int32_t y0, h;
};

struct scan_ctx {
	struct alpha_rect *out;   /* 输出数组，可为 NULL */
	size_t cap;               /* out 容量 */
	size_t n;                 /* 已写出数（可能超过 cap，用于精确计数） */
};

static void emit(struct scan_ctx *c, const struct seg *s)
{
	if (c->n < c->cap) {
		c->out[c->n].x = s->x;
		c->out[c->n].y = s->y0;
		c->out[c->n].w = s->w;
		c->out[c->n].h = s->h;
	}
	c->n++;
}

size_t alpha_region_scan(const uint8_t *pixels, int32_t width, int32_t height,
			 size_t stride, uint8_t threshold,
			 struct alpha_rect *rects, size_t max_rects)
{
	if (!pixels || width <= 0 || height <= 0)
		return 0;
	if (stride == 0)
		stride = (size_t)width * 4;

	const size_t segcap = (size_t)(width / 2) + 2;

	struct seg *pend = calloc(segcap, sizeof(*pend));   /* 上一行仍活跃的段 */
	struct seg *newp = calloc(segcap, sizeof(*newp));
	struct seg *row  = calloc(segcap, sizeof(*row));
	struct scan_ctx ctx = { rects, rects ? max_rects : 0, 0 };

	/* 计数模式：先按 segcap 试，满了就整体扩容（极少发生：segcap 已是理论上限） */
	if (!pend || !newp || !row) {
		free(pend); free(newp); free(row);
		return 0;
	}

	size_t npend = 0;

	for (int32_t y = 0; y < height; y++) {
		const uint8_t *p = pixels + (size_t)y * stride;
		size_t nrow = 0;
		int32_t run = -1;

		for (int32_t x = 0; x < width; x++) {
			int opaque = p[(size_t)x * 4 + 3] > threshold;
			if (opaque) {
				if (run < 0)
					run = x;
			} else if (run >= 0) {
				if (nrow < segcap) {
					row[nrow].x = run;
					row[nrow].w = x - run;
				}
				nrow++;
				run = -1;
			}
		}
		if (run >= 0) {
			if (nrow < segcap) {
				row[nrow].x = run;
				row[nrow].w = width - run;
			}
			nrow++;
		}

		/* 上一行 pend 与本行 row 双指针归并：
		 * 同 x/w → 长高；pend 独有 → 收尾写出；row 独有 → 新段 */
		size_t a = 0, b = 0, k = 0;
		while (a < npend || b < nrow) {
			int same = (a < npend && b < nrow &&
				    pend[a].x == row[b].x && pend[a].w == row[b].w);
			int pend_left = (a < npend && b >= nrow);
			int pend_first = (a < npend && b < nrow && pend[a].x < row[b].x);

			if (same) {
				newp[k] = pend[a];
				newp[k].h += 1;
				k++; a++; b++;
			} else if (pend_left || pend_first) {
				emit(&ctx, &pend[a]);
				a++;
			} else {
				newp[k].x = row[b].x;
				newp[k].w = row[b].w;
				newp[k].y0 = y;
				newp[k].h = 1;
				k++; b++;
			}
		}

		struct seg *t = pend; pend = newp; newp = t;
		npend = k;
	}

	for (size_t i = 0; i < npend; i++)
		emit(&ctx, &pend[i]);

	free(pend);
	free(newp);
	free(row);

	return ctx.n;
}

/* ---------------------------------------------------------------- build */

struct wl_region *alpha_region_build(struct wl_compositor *comp,
				    const uint8_t *pixels, int32_t width, int32_t height,
				    size_t stride, uint8_t threshold,
				    size_t max_rects)
{
	if (!comp)
		return NULL;
	if (max_rects == 0)
		max_rects = 4096;

	struct wl_region *region = wl_compositor_create_region(comp);
	if (!region)
		return NULL;

	size_t need = alpha_region_scan(pixels, width, height, stride, threshold,
					NULL, 0);
	if (need == 0)
		return region; /* 空 region：整个 surface 不接收输入 */

	struct alpha_rect *rects = malloc(need * sizeof(*rects));
	if (!rects)
		return region;

	size_t n = alpha_region_scan(pixels, width, height, stride, threshold,
				     rects, need);
	if (n > need)
		n = need;
	if (n > max_rects) {
		/* 超出预算：只保留前 max_rects 条（面积会不足，典型做法是提阈值） */
		n = max_rects;
	}

	for (size_t i = 0; i < n; i++)
		wl_region_add(region, rects[i].x, rects[i].y,
			      rects[i].w, rects[i].h);

	free(rects);
	return region;
}

void alpha_region_apply(struct wl_surface *surface, struct wl_compositor *comp,
			const uint8_t *pixels, int32_t width, int32_t height,
			size_t stride, uint8_t threshold, size_t max_rects)
{
	if (!surface || !comp)
		return;
	struct wl_region *r = alpha_region_build(comp, pixels, width, height,
						 stride, threshold, max_rects);
	if (!r)
		return;
	wl_surface_set_input_region(surface, r);
	wl_region_destroy(r);
}

size_t alpha_region_area(const struct alpha_rect *rects, size_t n)
{
	size_t a = 0;
	for (size_t i = 0; i < n; i++)
		if (rects[i].w > 0 && rects[i].h > 0)
			a += (size_t)rects[i].w * (size_t)rects[i].h;
	return a;
}
