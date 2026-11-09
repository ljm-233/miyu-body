/* input-probe.c — 验证 wl_surface.set_input_region() 在 niri 上生效
 *
 * 场景：一个 400x400 的 overlay 浮层，中间画半径 150 的红圆，圆外 alpha=0。
 *       input region 只包含圆（按 alpha 扫描出来的矩形带），
 *       圆外的点击应该穿透到下面那一层。
 *
 * 为了能自动验证「穿透到下面」，同进程里先建一个 600x600 的 overlay 记录器
 * （不设 input region = 整块都接受），再建 400x400 的探针盖在上面。
 * 一个 wl_pointer 同时看两个 surface 的 enter/button，谁收到就说明点击落在谁身上。
 *
 *   点圆心 → probe 收到（被浮层吃掉）
 *   点四角 → recorder 收到（穿透下去了）
 *
 * 退出：Ctrl-C，或 SIGTERM。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>
#include <wayland-client.h>

#include "alpha_region.h"
#include "gen/layer-shell-client.h"

#define PANEL_PROBE    400
#define PANEL_RECORDER 600
/* 两块屏里 eDP-1 逻辑位置是 1920,0；探针在它左上角偏移 (160,160) 处，
 * 记录器在 (60,60) 处 —— 探针正好落在记录器正中间。 */
#define MARGIN_PROBE    160
#define MARGIN_RECORDER 60
#define CIRCLE_R        150.0

static struct wl_display *display;
static struct wl_compositor *compositor;
static struct wl_shm *shm;
static struct zwlr_layer_shell_v1 *layer_shell;
static struct wl_seat *seat;
static struct wl_pointer *pointer;
static struct wl_output *target_output; /* eDP-1 */
static volatile sig_atomic_t running = 1;

static struct wl_surface *ptr_surface;  /* 指针当前进入的 surface */
static double ptr_sx, ptr_sy;

struct panel {
	struct wl_surface *surface;
	struct zwlr_layer_surface_v1 *layer;
	struct wl_buffer *buffer;
	uint32_t *px;
	int w, h;
	const char *tag;
	int configured;
	int use_alpha_region;
	size_t region_rects;
};

static struct panel probe, recorder;

/* ------------------------------------------------------------------ util */

static void on_signal(int sig)
{
	(void)sig;
	running = 0;
}

static int create_memfd(size_t size)
{
	int fd = memfd_create("input-probe", MFD_CLOEXEC);
	if (fd < 0)
		return -1;
	if (ftruncate(fd, (off_t)size) < 0) {
		close(fd);
		return -1;
	}
	return fd;
}

static const char *tag_of(struct wl_surface *s)
{
	if (s == probe.surface)
		return "probe";
	if (s == recorder.surface)
		return "recorder";
	return "other";
}

/* 画图：中间一个不透明红圆（带 1px 抗锯齿），圆外 alpha=0。
 * wl_shm 的 ARGB8888 要求预乘 alpha，红圆 = (A,R,G,B) = (a, a, 0, 0)。 */
static void draw_circle(uint32_t *px, int w, int h, double R, uint32_t rgb)
{
	memset(px, 0, (size_t)w * h * 4);
	double cx = w / 2.0, cy = h / 2.0;
	uint8_t r8 = (rgb >> 16) & 0xff, g8 = (rgb >> 8) & 0xff, b8 = rgb & 0xff;

	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
			double d2 = dx * dx + dy * dy;
			uint8_t a = 0;

			if (d2 <= (R - 1.0) * (R - 1.0)) {
				a = 255;
			} else if (d2 < R * R) {
				double t = R - sqrt(d2);   /* 0..1 */
				a = (uint8_t)(t * 255.0);
			}
			if (!a)
				continue;
			uint32_t pa = a, pr = (uint32_t)r8 * a / 255;
			uint32_t pg = (uint32_t)g8 * a / 255;
			uint32_t pb = (uint32_t)b8 * a / 255;
			px[(size_t)y * w + x] = (pa << 24) | (pr << 16) | (pg << 8) | pb;
		}
	}
}

/* ------------------------------------------------------------- 事件监听 */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial,
		      struct wl_surface *surface, wl_fixed_t sx, wl_fixed_t sy)
{
	(void)d; (void)p; (void)serial;
	ptr_surface = surface;
	ptr_sx = wl_fixed_to_double(sx);
	ptr_sy = wl_fixed_to_double(sy);
	fprintf(stderr, "[ptr] enter  %-8s local=(%.1f,%.1f)\n",
		tag_of(surface), ptr_sx, ptr_sy);
	fflush(stderr);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial,
		      struct wl_surface *surface)
{
	(void)d; (void)p; (void)serial;
	fprintf(stderr, "[ptr] leave  %-8s\n", tag_of(surface));
	fflush(stderr);
	if (surface == ptr_surface)
		ptr_surface = NULL;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time,
		       wl_fixed_t sx, wl_fixed_t sy)
{
	(void)d; (void)p; (void)time;
	ptr_sx = wl_fixed_to_double(sx);
	ptr_sy = wl_fixed_to_double(sy);
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial,
		       uint32_t time, uint32_t button, uint32_t state)
{
	(void)d; (void)p; (void)time; (void)serial;
	if (state != WL_POINTER_BUTTON_STATE_PRESSED)
		return;
	const char *t = ptr_surface ? tag_of(ptr_surface) : "none";
	fprintf(stderr, "[ptr] BUTTON %-8s btn=%u local=(%.1f,%.1f)\n",
		t, button, ptr_sx, ptr_sy);
	fflush(stderr);
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time,
		     uint32_t axis, wl_fixed_t v)
{
	(void)d; (void)p; (void)time; (void)axis; (void)v;
}

static const struct wl_pointer_listener pointer_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
};

static void seat_capabilities(void *d, struct wl_seat *s, uint32_t caps)
{
	(void)d;
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(s);
		wl_pointer_add_listener(pointer, &pointer_listener, NULL);
		fprintf(stderr, "[seat] pointer 已绑定\n");
	}
}

static void seat_name(void *d, struct wl_seat *s, const char *name)
{
	(void)d; (void)s;
	fprintf(stderr, "[seat] name=%s\n", name ? name : "(null)");
}

static const struct wl_seat_listener seat_listener = {
	.capabilities = seat_capabilities,
	.name = seat_name,
};

/* output：只为了把 surface 摆到 eDP-1 上 */
static void out_geometry(void *d, struct wl_output *o, int32_t x, int32_t y,
			 int32_t pw, int32_t ph, int32_t subpixel,
			 const char *make, const char *model, int32_t transform)
{
	(void)d; (void)o; (void)pw; (void)ph; (void)subpixel;
	(void)make; (void)model; (void)transform;
	fprintf(stderr, "[output] geometry at %d,%d\n", x, y);
}

static void out_mode(void *d, struct wl_output *o, uint32_t flags,
		     int32_t w, int32_t h, int32_t refresh)
{
	(void)d; (void)o; (void)refresh;
	if (flags & WL_OUTPUT_MODE_CURRENT)
		fprintf(stderr, "[output] mode %dx%d\n", w, h);
}

static void out_done(void *d, struct wl_output *o) { (void)d; (void)o; }

static void out_scale(void *d, struct wl_output *o, int32_t factor)
{
	(void)d; (void)o;
	fprintf(stderr, "[output] scale=%d\n", factor);
}

static void out_name(void *d, struct wl_output *o, const char *name)
{
	(void)d;
	fprintf(stderr, "[output] name=%s\n", name);
	if (name && strcmp(name, "eDP-1") == 0 && !target_output)
		target_output = o;
}

static void out_desc(void *d, struct wl_output *o, const char *desc)
{
	(void)d; (void)o; (void)desc;
}

static const struct wl_output_listener output_listener = {
	.geometry = out_geometry,
	.mode = out_mode,
	.done = out_done,
	.scale = out_scale,
	.name = out_name,
	.description = out_desc,
};

/* -------------------------------------------------------------- 面板 */

static struct wl_buffer *make_buffer(int w, int h, uint32_t **px_out)
{
	size_t size = (size_t)w * h * 4;
	int fd = create_memfd(size);
	if (fd < 0) {
		fprintf(stderr, "memfd 失败: %s\n", strerror(errno));
		return NULL;
	}
	void *map = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr, "mmap 失败: %s\n", strerror(errno));
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, (int32_t)size);
	struct wl_buffer *buf = wl_shm_pool_create_buffer(
		pool, 0, w, h, w * 4, WL_SHM_FORMAT_ARGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	*px_out = map;
	return buf;
}

static void panel_commit(struct panel *p)
{
	wl_surface_attach(p->surface, p->buffer, 0, 0);
	wl_surface_damage_buffer(p->surface, 0, 0, p->w, p->h);

	if (p->use_alpha_region) {
		/* 关键的那一帧：只有 alpha>8 的像素组成 input region */
		size_t need = alpha_region_scan((const uint8_t *)p->px, p->w, p->h,
						0, 8, NULL, 0);
		struct alpha_rect *rects = calloc(need ? need : 1, sizeof(*rects));
		size_t got = alpha_region_scan((const uint8_t *)p->px, p->w, p->h,
					       0, 8, rects, need);
		p->region_rects = got;

		struct wl_region *r = alpha_region_build(
			compositor, (const uint8_t *)p->px, p->w, p->h, 0, 8, 8192);
		if (r) {
			fprintf(stderr,
				"[region] %s: %zu 条矩形, 面积 %zu px（surface %dx%d 共 %d）"
				" → 调 wl_surface.set_input_region\n",
				p->tag, got, alpha_region_area(rects, got),
				p->w, p->h, p->w * p->h);
			wl_surface_set_input_region(p->surface, r);
			wl_region_destroy(r);
		}
		free(rects);
	}

	wl_surface_commit(p->surface);
}

static void ls_configure(void *data, struct zwlr_layer_surface_v1 *ls,
			 uint32_t serial, uint32_t w, uint32_t h)
{
	struct panel *p = data;
	zwlr_layer_surface_v1_ack_configure(ls, serial);
	if (!p->configured) {
		p->configured = 1;
		fprintf(stderr, "[layer] %s configured %ux%u\n", p->tag, w, h);
		panel_commit(p);
	}
}

static void ls_closed(void *data, struct zwlr_layer_surface_v1 *ls)
{
	(void)ls;
	struct panel *p = data;
	fprintf(stderr, "[layer] %s closed by compositor\n", p->tag);
	running = 0;
}

static const struct zwlr_layer_surface_v1_listener ls_listener = {
	.configure = ls_configure,
	.closed = ls_closed,
};

static void panel_create(struct panel *p, const char *tag, int size, int margin,
			 int use_alpha_region, uint32_t rgb)
{
	memset(p, 0, sizeof(*p));
	p->tag = tag;
	p->w = p->h = size;
	p->use_alpha_region = use_alpha_region;

	p->surface = wl_compositor_create_surface(compositor);
	p->buffer = make_buffer(p->w, p->h, &p->px);
	draw_circle(p->px, p->w, p->h,
		    use_alpha_region ? CIRCLE_R : (double)size * 0.5, rgb);

	p->layer = zwlr_layer_shell_v1_get_layer_surface(
		layer_shell, p->surface, target_output,
		ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, tag);
	zwlr_layer_surface_v1_add_listener(p->layer, &ls_listener, p);

	zwlr_layer_surface_v1_set_anchor(
		p->layer, ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP |
				  ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT);
	zwlr_layer_surface_v1_set_margin(p->layer, margin, 0, 0, margin);
	zwlr_layer_surface_v1_set_size(p->layer, p->w, p->h);
	zwlr_layer_surface_v1_set_exclusive_zone(p->layer, -1);
	zwlr_layer_surface_v1_set_keyboard_interactivity(
		p->layer, ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
	wl_surface_commit(p->surface);
}

/* ------------------------------------------------------------- registry */

static void reg_global(void *d, struct wl_registry *r, uint32_t name,
		       const char *iface, uint32_t version)
{
	(void)d;
	if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(r, name, &wl_compositor_interface,
					      version < 4 ? version : 4);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, zwlr_layer_shell_v1_interface.name)) {
		layer_shell = wl_registry_bind(r, name,
					       &zwlr_layer_shell_v1_interface,
					       version < 4 ? version : 4);
	} else if (!strcmp(iface, wl_seat_interface.name)) {
		seat = wl_registry_bind(r, name, &wl_seat_interface, 1);
		wl_seat_add_listener(seat, &seat_listener, NULL);
	} else if (!strcmp(iface, wl_output_interface.name)) {
		struct wl_output *o = wl_registry_bind(
			r, name, &wl_output_interface, version < 4 ? version : 4);
		wl_output_add_listener(o, &output_listener, NULL);
	}
}

static void reg_remove(void *d, struct wl_registry *r, uint32_t name)
{
	(void)d; (void)r; (void)name;
}

static const struct wl_registry_listener registry_listener = {
	.global = reg_global,
	.global_remove = reg_remove,
};

/* ----------------------------------------------------------------- main */

int main(void)
{
	signal(SIGINT, on_signal);
	signal(SIGTERM, on_signal);

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "wl_display_connect 失败\n");
		return 1;
	}
	fprintf(stderr, "[main] 连上 wayland，protocols: compositor/shm/layer-shell/seat/output\n");

	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &registry_listener, NULL);
	wl_display_roundtrip(display);   /* 绑定 globals */
	wl_display_roundtrip(display);   /* 收 output 的 name/geometry */

	if (!compositor || !shm || !layer_shell) {
		fprintf(stderr, "[main] 缺关键 global: compositor=%p shm=%p layer_shell=%p\n",
			(void *)compositor, (void *)shm, (void *)layer_shell);
		return 1;
	}
	fprintf(stderr, "[main] target_output=%s\n",
		target_output ? "eDP-1" : "(未找到，用 focused output)");

	/* 先建记录器（600x600，整块都收点击），再建探针（400x400，只有圆收点击）。
	 * niri 同层内后创建的压在上面。 */
	panel_create(&recorder, "recorder", PANEL_RECORDER, MARGIN_RECORDER, 0,
		     0x2050a0);
	panel_create(&probe, "probe", PANEL_PROBE, MARGIN_PROBE, 1, 0xff0000);

	fprintf(stderr,
		"[main] 就绪：probe %dx%d @ eDP-1+(%d,%d)，圆 R=%.0f；"
		"recorder %dx%d @ eDP-1+(%d,%d)\n",
		PANEL_PROBE, PANEL_PROBE, MARGIN_PROBE, MARGIN_PROBE, CIRCLE_R,
		PANEL_RECORDER, PANEL_RECORDER, MARGIN_RECORDER, MARGIN_RECORDER);
	fflush(stderr);

	/* 用 poll 带超时，这样收到 SIGTERM/SIGINT 后能在 200ms 内退出，
	 * 不会卡在 wl_display_dispatch 的阻塞读里把窗口留在屏幕上。 */
	while (running) {
		wl_display_dispatch_pending(display);
		wl_display_flush(display);

		struct pollfd pfd = { .fd = wl_display_get_fd(display),
				      .events = POLLIN };
		int pr = poll(&pfd, 1, 200);
		if (pr < 0) {
			if (errno == EINTR)
				continue;
			fprintf(stderr, "[main] poll 失败: %s\n", strerror(errno));
			break;
		}
		if (pr > 0 && (pfd.revents & POLLIN)) {
			if (wl_display_dispatch(display) < 0) {
				fprintf(stderr, "[main] dispatch 失败: %s\n",
					strerror(errno));
				break;
			}
		}
	}

	fprintf(stderr, "[main] 退出，清理 surface\n");
	if (probe.layer)
		zwlr_layer_surface_v1_destroy(probe.layer);
	if (probe.surface)
		wl_surface_destroy(probe.surface);
	if (recorder.layer)
		zwlr_layer_surface_v1_destroy(recorder.layer);
	if (recorder.surface)
		wl_surface_destroy(recorder.surface);
	wl_display_flush(display);
	wl_display_disconnect(display);
	return 0;
}
