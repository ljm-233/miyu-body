/* clicker.c — 用 wlr-virtual-pointer 注入一次真实点击（无需 root、无需 ydotool）
 *
 * 用法: ./clicker GX GY [EXTENT_W EXTENT_H] [--no-click]
 *   GX/GY : 全局逻辑坐标（layout 坐标，eDP-1 从 (1920,0) 起）
 *   EXTENT: motion_absolute 的归一化分母，默认取整块 layout 3626x1080
 *           （wlroots 语义：x/x_extent 映射到 layout 宽度）
 *
 * 输出到 stderr：发出了哪些请求。实际落点看 input-probe 那边的 [ptr] 日志。
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
		fprintf(stderr, "[clicker] 绑定 virtual-pointer-manager v%u\n",
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

int main(int argc, char **argv)
{
	if (argc < 3) {
		fprintf(stderr, "用法: %s GX GY [EXTENT_W EXTENT_H] [--no-click]\n",
			argv[0]);
		return 2;
	}
	double gx = atof(argv[1]);
	double gy = atof(argv[2]);
	double ew = 3626, eh = 1080;
	int do_click = 1;
	for (int i = 3; i < argc; i++) {
		if (!strcmp(argv[i], "--no-click"))
			do_click = 0;
		else if (i == 3)
			ew = atof(argv[i]);
		else if (i == 4)
			eh = atof(argv[i]);
	}

	dpy = wl_display_connect(NULL);
	if (!dpy) {
		fprintf(stderr, "[clicker] wl_display_connect 失败\n");
		return 1;
	}
	struct wl_registry *reg = wl_display_get_registry(dpy);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(dpy);
	wl_display_roundtrip(dpy);

	if (!mgr) {
		fprintf(stderr, "[clicker] 合成器没有 zwlr_virtual_pointer_manager_v1\n");
		return 1;
	}

	struct zwlr_virtual_pointer_v1 *vp =
		zwlr_virtual_pointer_manager_v1_create_virtual_pointer(mgr, NULL);
	if (!vp) {
		fprintf(stderr, "[clicker] create_virtual_pointer 返回 NULL\n");
		return 1;
	}

	uint32_t t = now_ms();
	zwlr_virtual_pointer_v1_motion_absolute(
		vp, t, wl_fixed_from_double(gx), wl_fixed_from_double(gy),
		wl_fixed_from_double(ew), wl_fixed_from_double(eh));
	zwlr_virtual_pointer_v1_frame(vp);
	wl_display_flush(dpy);
	fprintf(stderr, "[clicker] motion_absolute(%.1f,%.1f) extent=%.0fx%.0f\n",
		gx, gy, ew, eh);
	usleep(80000);
	wl_display_roundtrip(dpy);

	if (do_click) {
		uint32_t t2 = now_ms();
		zwlr_virtual_pointer_v1_button(vp, t2, BTN_LEFT,
					       WL_POINTER_BUTTON_STATE_PRESSED);
		zwlr_virtual_pointer_v1_frame(vp);
		wl_display_flush(dpy);
		usleep(50000);
		uint32_t t3 = now_ms();
		zwlr_virtual_pointer_v1_button(vp, t3, BTN_LEFT,
					       WL_POINTER_BUTTON_STATE_RELEASED);
		zwlr_virtual_pointer_v1_frame(vp);
		wl_display_flush(dpy);
		fprintf(stderr, "[clicker] BTN_LEFT pressed+released\n");
		usleep(50000);
	}

	zwlr_virtual_pointer_v1_destroy(vp);
	zwlr_virtual_pointer_manager_v1_destroy(mgr);
	wl_display_flush(dpy);
	wl_display_disconnect(dpy);
	return 0;
}
