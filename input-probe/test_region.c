/* test_region.c — alpha_region 的纯逻辑单测（不连 Wayland）
 *
 * 喂四张构造好的 alpha 图，断言矩形数量与覆盖面积。
 * 面积断言用「像素级真值」对比：先独立数一遍 alpha>th 的像素数，
 * 再数矩形带覆盖的像素数，两者必须完全相等（矩形带不能多也不能少）。
 */
#define _GNU_SOURCE
#include "alpha_region.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails = 0;
static int checks = 0;

#define CHECK(cond, fmt, ...)                                                \
	do {                                                                 \
		checks++;                                                    \
		if (!(cond)) {                                               \
			fails++;                                             \
			printf("  FAIL: " fmt "\n", ##__VA_ARGS__);          \
		} else {                                                     \
			printf("  ok  : " fmt "\n", ##__VA_ARGS__);          \
		}                                                            \
	} while (0)

/* 建一张 w*h 的 RGBA 图，全部 alpha=0，然后交给 fill 回调画 */
static uint8_t *img_new(int w, int h)
{
	uint8_t *p = calloc((size_t)w * h * 4, 1);
	return p;
}

static void px(uint8_t *img, int w, int x, int y, uint8_t a)
{
	uint8_t *p = img + ((size_t)y * w + x) * 4;
	p[0] = 255; p[1] = 0; p[2] = 0; p[3] = a;
}

/* 独立真值：直接数 alpha>th 的像素 */
static size_t truth_area(const uint8_t *img, int w, int h, uint8_t th)
{
	size_t n = 0;
	for (int i = 0; i < w * h; i++)
		if (img[(size_t)i * 4 + 3] > th)
			n++;
	return n;
}

static void run_case(const char *name, uint8_t *img, int w, int h, uint8_t th,
		     size_t min_rects, size_t max_rects_expect)
{
	printf("[%s] %dx%d th=%u\n", name, w, h, th);

	size_t need = alpha_region_scan(img, w, h, 0, th, NULL, 0);
	struct alpha_rect *r = calloc(need ? need : 1, sizeof(*r));
	size_t got = alpha_region_scan(img, w, h, 0, th, r, need);

	size_t truth = truth_area(img, w, h, th);
	size_t area = alpha_region_area(r, got);

	CHECK(got == need, "计数与写入一致：need=%zu got=%zu", need, got);
	CHECK(area == truth, "覆盖面积=真值：%zu vs %zu", area, truth);
	CHECK(need >= min_rects, "矩形数 >= %zu（实际 %zu）", min_rects, need);
	if (max_rects_expect)
		CHECK(need <= max_rects_expect, "矩形数 <= %zu（实际 %zu）",
		      max_rects_expect, need);

	/* 验矩形没有越界 */
	int bad = 0;
	for (size_t i = 0; i < got; i++) {
		if (r[i].x < 0 || r[i].y < 0 || r[i].w <= 0 || r[i].h <= 0 ||
		    r[i].x + r[i].w > w || r[i].y + r[i].h > h)
			bad++;
	}
	CHECK(bad == 0, "无越界矩形（越界 %d 条）", bad);

	/* 独立验：矩形带与真值 bitmap 完全相等（不重不漏） */
	uint8_t *mask = calloc((size_t)w * h, 1);
	for (size_t i = 0; i < got; i++)
		for (int y = r[i].y; y < r[i].y + r[i].h; y++)
			for (int x = r[i].x; x < r[i].x + r[i].w; x++)
				mask[(size_t)y * w + x]++;
	int dup = 0, miss = 0, extra = 0;
	for (int i = 0; i < w * h; i++) {
		int want = img[(size_t)i * 4 + 3] > th;
		if (mask[i] > 1) dup++;
		if (want && mask[i] == 0) miss++;
		if (!want && mask[i] > 0) extra++;
	}
	CHECK(dup == 0 && miss == 0 && extra == 0,
	      "逐像素一致：重复=%d 漏=%d 多=%d", dup, miss, extra);

	free(mask);
	free(r);
	free(img);
	printf("\n");
}

int main(void)
{
	printf("=== alpha_region 单测 ===\n\n");

	/* 1) 全透明：0 矩形，0 面积 → 整个 surface 穿透 */
	{
		int w = 64, h = 64;
		uint8_t *img = img_new(w, h);
		run_case("全透明", img, w, h, 8, 0, 0);
	}

	/* 2) 全不透明：按竖合并应该正好 1 条矩形（整块） */
	{
		int w = 64, h = 64;
		uint8_t *img = img_new(w, h);
		memset(img, 0, (size_t)w * h * 4);
		for (int i = 0; i < w * h; i++)
			img[(size_t)i * 4 + 3] = 255;
		run_case("全不透明", img, w, h, 8, 1, 1);
	}

	/* 3) 一个洞：不透明底上一块透明方洞 */
	{
		int w = 64, h = 64;
		uint8_t *img = img_new(w, h);
		for (int i = 0; i < w * h; i++)
			img[(size_t)i * 4 + 3] = 255;
		for (int y = 20; y < 44; y++)
			for (int x = 20; x < 44; x++)
				img[((size_t)y * w + x) * 4 + 3] = 0;
		/* 洞上方 1 条 + 洞左右 2 条（跨 24 行，竖合并成 2 条）+ 洞下方 1 条 = 4 */
		run_case("中心一个洞", img, w, h, 8, 4, 4);
	}

	/* 4) 一条 45° 斜线（宽 3）：每行一段，竖方向会合并成若干台阶 */
	{
		int w = 64, h = 64;
		uint8_t *img = img_new(w, h);
		for (int y = 0; y < h; y++) {
			int x = y;
			for (int k = 0; k < 3; k++)
				if (x + k < w)
					px(img, w, x + k, y, 255);
		}
		/* 每行 1 段，最多 64 条；竖合并后应显著减少但 >1 */
		run_case("45度斜线", img, w, h, 8, 2, 64);
	}

	/* 5) 阈值边界：alpha 恰好 = 阈值时应视为透明 */
	{
		int w = 8, h = 8;
		uint8_t *img = img_new(w, h);
		for (int i = 0; i < w * h; i++)
			img[(size_t)i * 4 + 3] = 8; /* 等于阈值 */
		printf("[阈值边界] alpha=8 th=8 → 应 0 条\n");
		size_t n = alpha_region_scan(img, w, h, 0, 8, NULL, 0);
		checks++;
		if (n != 0) { fails++; printf("  FAIL: 期望 0 条，实际 %zu\n", n); }
		else printf("  ok  : 0 条（alpha>th 才不透明）\n");
		for (int i = 0; i < w * h; i++)
			img[(size_t)i * 4 + 3] = 9;
		size_t n2 = alpha_region_scan(img, w, h, 0, 8, NULL, 0);
		checks++;
		if (n2 != 1) { fails++; printf("  FAIL: alpha=9 期望 1 条，实际 %zu\n", n2); }
		else printf("  ok  : alpha=9 → 1 条\n");
		free(img);
		printf("\n");
	}

	/* 6) 抗锯齿圆（radius 30）— 模拟真实桌宠贴图，检查面积接近 pi*r^2 */
	{
		int w = 80, h = 80;
		uint8_t *img = img_new(w, h);
		double cx = 40, cy = 40, R = 30;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
				double d = dx * dx + dy * dy;
				if (d <= (R - 1) * (R - 1)) {
					px(img, w, x, y, 255);
				} else if (d < R * R) {
					double t = 1.0 - (__builtin_sqrt(d) - (R - 1));
					if (t < 0) t = 0;
					px(img, w, x, y, (uint8_t)(t * 255));
				}
			}
		}
		printf("[抗锯齿圆] r=30\n");
		size_t need = alpha_region_scan(img, w, h, 0, 8, NULL, 0);
		struct alpha_rect *r = calloc(need, sizeof(*r));
		size_t got = alpha_region_scan(img, w, h, 0, 8, r, need);
		size_t area = alpha_region_area(r, got);
		size_t truth = truth_area(img, w, h, 8);
		double ideal = 3.14159265 * 30 * 30;
		printf("  矩形 %zu 条，覆盖 %zu px（真值 %zu，理想圆 %.0f）\n",
		       got, area, truth, ideal);
		checks++;
		if (area != truth) { fails++; printf("  FAIL: 面积不等于真值\n"); }
		else printf("  ok  : 面积=真值\n");
		checks++;
		if (area > ideal * 1.15 || area < ideal * 0.85) {
			fails++;
			printf("  FAIL: 面积偏离理想圆超过 15%%\n");
		} else printf("  ok  : 面积在 ideal±15%% 内\n");
		free(r);
		free(img);
		printf("\n");
	}

	printf("=== %d 项检查，%d 失败 ===\n", checks, fails);
	if (fails) { printf("FAIL\n"); return 1; }
	printf("PASS\n");
	return 0;
}
