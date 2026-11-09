/* draginject.c — 用 niri 自带的 zwlr_virtual_pointer_v1 注入一次「按下 + 拖 + 松开」
 *
 * 抄自 ../input-probe/clicker.c（那边只点击不拖），多了分步位移。
 * 只读引用那边的协议代码和生成的客户端头，不改 input-probe/ 任何文件。
 *
 * 用法:
 *   ./draginject GX GY [DX DY [STEPS [STEP_MS]]]
 *     GX/GY   : 按下点的**全局 layout 逻辑坐标**（eDP-1 从 (1920,0) 起）
 *     DX/DY   : 拖动总位移（逻辑 px），默认 0 0（只按一下不拖）
 *     STEPS   : 分几步走完，默认 20（分步是为了让窗口每步都能跟上：
 *               一步走太远，指针会先跑出她的 input region，那一帧就收不到 motion）
 *     STEP_MS : 每步之间等多少毫秒，默认 45
 *
 * 归一化分母默认整块 layout 3626x1080（= 1920 的 HDMI + 1706 的 eDP-1）。
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "gen/virtual-pointer-client.h"

#define BTN_LEFT 0x110

static struct wl_display *dpy;
static struct zwlr_virtual_pointer_manager_v1 *mgr;

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t ver)
{
	(void)d;
	if (!strcmp(iface, zwlr_virtual_pointer_manager_v1_interface.name)) {
		mgr = wl_registry_bind(r, name,
				       &zwlr_virtual_pointer_manager_v1_interface,
				       ver < 2 ? ver : 2);
		fprintf(stderr, "[drag] 绑定 virtual-pointer-manager v%u\n",
			ver < 2 ? ver : 2);
	}
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

static uint32_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static struct zwlr_virtual_pointer_v1 *vp;
static double EW = 3626, EH = 1080;

static void move_to(double gx, double gy)
{
	zwlr_virtual_pointer_v1_motion_absolute(
		vp, now_ms(), wl_fixed_from_double(gx), wl_fixed_from_double(gy),
		wl_fixed_from_double(EW), wl_fixed_from_double(EH));
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(dpy);
}

static void btn(uint32_t state)
{
	zwlr_virtual_pointer_v1_button(vp, now_ms(), BTN_LEFT, state);
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(dpy);
}

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr,
			"用法: %s GX GY [DX DY [STEPS [STEP_MS]]]\n", argv[0]);
		return 2;
	}
	double gx = atof(argv[1]);
	double gy = atof(argv[2]);
	double dx = argc > 3 ? atof(argv[3]) : 0.0;
	double dy = argc > 4 ? atof(argv[4]) : 0.0;
	int steps = argc > 5 ? atoi(argv[5]) : 20;
	int step_ms = argc > 6 ? atoi(argv[6]) : 45;
	if (steps < 1) steps = 1;
	int rel = (argc > 7 && !strcmp(argv[7], "--rel"));

	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "[drag] wl_display_connect 失败\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);

	if (!mgr) {
		fprintf(stderr, "[drag] 合成器没有 zwlr_virtual_pointer_manager_v1\n");
		return 1;
	}

	vp = zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, NULL);
	if (!vp) {
		fprintf(stderr, "[drag] create_virtual_pointer 返回 NULL\n");
		return 1;
	}

	/* 1) 先把指针挪到按点并停稳（合成器要先发 enter，我们的程序才认识这个指针） */
	move_to(gx, gy);
	usleep(120000);

	/* 2) 按下 */
	btn(WL_POINTER_BUTTON_STATE_PRESSED);
	fprintf(stderr, "[drag] 按下 (%0.1f,%0.1f)\n", gx, gy);
	usleep(120000);

	/* 3) 分步走完位移
	 *  --rel：用 relative motion（和真鼠标一样的相对增量）。
	 *          absolute 是「把光标瞬移过去」，合成器那边 surface 位置还没跟上时
	 *          会算出一大坨假位移 —— 测出来的东西不代表真鼠标。 */
	for (int i = 1; i <= steps; i++) {
		if (rel) {
			zwlr_virtual_pointer_v1_motion(
				vp, now_ms(),
				wl_fixed_from_double(dx / (double)steps),
				wl_fixed_from_double(dy / (double)steps));
			zwlr_virtual_pointer_v1_frame(vp);
			wl_display_flush(dpy);
			usleep((useconds_t)step_ms * 1000);
		} else {
			double fx = gx + dx * (double)i / (double)steps;
			double fy = gy + dy * (double)i / (double)steps;
			move_to(fx, fy);
			usleep((useconds_t)step_ms * 1000);
		}
	}
	fprintf(stderr, "[drag] 拖到 (%0.1f,%0.1f)，位移 (%0.1f,%0.1f) 分 %d 步\n",
		gx + dx, gy + dy, dx, dy, steps);
	usleep(150000);

	/* 4) 松开 */
	btn(WL_POINTER_BUTTON_STATE_RELEASED);
	fprintf(stderr, "[drag] 松开\n");
	usleep(120000);

	zwlr_virtual_pointer_v1_destroy(vp);
	zwlr_virtual_pointer_manager_v1_destroy(mgr);
	wl_display_flush(dpy);
	wl_display_disconnect(dpy);
	return 0;
}
