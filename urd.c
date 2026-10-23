// urd — pasek i tapeta norns, rysowane do buforów wl_shm (pixman + fcft)
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wchar.h>
#include <fcft/fcft.h>
#include <pixman.h>
#include <wayland-cursor.h>
#include "norns.h"

#define PAD 8                       /* odstępy poziome na pasku */

static struct fcft_font *fonts[2];  /* po jednym na motyw */
static struct wl_pointer *pointer;
static struct wl_surface *ptr_surf, *cursor_surf;
static struct wl_cursor_image *cursor_img;
static int ptr_x;

/* ---------- bufory ---------- */

struct buf {
	struct wl_buffer *wb;
	pixman_image_t *img;
	void *data;
	size_t size;
};

/* compositor oddał bufor — sprzątamy (każdy bufor jest jednorazowy) */
static void buf_release(void *data, struct wl_buffer *wb)
{
	struct buf *b = data;
	pixman_image_unref(b->img);
	munmap(b->data, b->size);
	wl_buffer_destroy(wb);
	free(b);
}

static const struct wl_buffer_listener buf_listener = { .release = buf_release };

static struct buf *buf_new(int w, int h)
{
	int stride = w * 4;
	size_t size = (size_t)stride * h;
	int fd = memfd_create("urd", MFD_CLOEXEC);
	if (fd < 0)
		return NULL;
	if (ftruncate(fd, size) < 0) {
		close(fd);
		return NULL;
	}
	void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
	if (data == MAP_FAILED) {
		close(fd);
		return NULL;
	}
	struct wl_shm_pool *pool = wl_shm_create_pool(shm, fd, size);
	struct buf *b = calloc(1, sizeof *b);
	b->wb = wl_shm_pool_create_buffer(pool, 0, w, h, stride, WL_SHM_FORMAT_XRGB8888);
	wl_shm_pool_destroy(pool);
	close(fd);
	b->data = data;
	b->size = size;
	b->img = pixman_image_create_bits(PIXMAN_x8r8g8b8, w, h, data, stride);
	wl_buffer_add_listener(b->wb, &buf_listener, b);
	return b;
}

/* ---------- rysowanie ---------- */

static pixman_color_t pc(uint32_t rgb)
{
	return (pixman_color_t){
		.red   = ((rgb >> 16) & 0xff) * 0x101,
		.green = ((rgb >> 8) & 0xff) * 0x101,
		.blue  = (rgb & 0xff) * 0x101,
		.alpha = 0xffff,
	};
}

static void rect(pixman_image_t *img, int x, int y, int w, int h, uint32_t rgb)
{
	pixman_color_t c = pc(rgb);
	pixman_rectangle16_t r = { x, y, w, h };
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, img, &c, 1, &r);
}

/* Następny znak UTF-8 jako kod Unicode; przesuwa *s. */
static uint32_t next_cp(const char **s)
{
	mbstate_t st = { 0 };
	wchar_t wc;
	size_t n = mbrtowc(&wc, *s, MB_CUR_MAX, &st);
	if (n == (size_t)-1 || n == (size_t)-2 || n == 0) {
		(*s)++;
		return '?';
	}
	*s += n;
	return wc;
}

static int text_width(const char *s)
{
	int w = 0;
	while (*s) {
		const struct fcft_glyph *g = fcft_rasterize_char_utf32(fonts[theme_idx], next_cp(&s), FCFT_SUBPIXEL_NONE);
		if (g)
			w += g->advance.x;
	}
	return w;
}

/* Tekst wyśrodkowany w pionie w pasie [top, top+h); nie wychodzi za max_x. Zwraca x końca. */
static int text(pixman_image_t *img, int x, int top, int h, int max_x, const char *s, uint32_t rgb)
{
	struct fcft_font *f = fonts[theme_idx];
	int base = top + (h - (f->ascent + f->descent)) / 2 + f->ascent;
	pixman_color_t c = pc(rgb);
	pixman_image_t *fill = pixman_image_create_solid_fill(&c);

	while (*s) {
		const struct fcft_glyph *g = fcft_rasterize_char_utf32(f, next_cp(&s), FCFT_SUBPIXEL_NONE);
		if (!g)
			continue;
		if (x + g->advance.x > max_x)
			break;
		if (pixman_image_get_format(g->pix) == PIXMAN_a8r8g8b8)   /* kolorowy glif, np. emoji */
			pixman_image_composite32(PIXMAN_OP_OVER, g->pix, NULL, img,
			                         0, 0, 0, 0, x + g->x, base - g->y, g->width, g->height);
		else
			pixman_image_composite32(PIXMAN_OP_OVER, fill, g->pix, img,
			                         0, 0, 0, 0, x + g->x, base - g->y, g->width, g->height);
		x += g->advance.x;
	}
	pixman_image_unref(fill);
	return x;
}

int urd_bar_height(void)
{
	return fonts[theme_idx]->height + 8;
}

/*  [1][2][3]…[9]  tytuł okna ...............  Fri 23.10 21:41  [ GRUVBOX ] */
static void draw_bar(struct output *o, pixman_image_t *img, int w, int h)
{
	const struct theme *t = THEME;
	rect(img, 0, 0, w, h, t->bg);
	rect(img, 0, h - 1, w, 1, t->line);

	int x = 0;
	for (int i = 0; i < NTAGS; i++) {
		char label[2] = { '1' + i, 0 };
		int cw = text_width(label) + 2 * PAD;
		uint32_t fg = o->roots[i] ? t->fg : t->dim;
		if (i == o->tag) {
			rect(img, x, 0, cw, h - 1, t->focus);
			fg = t->bg;
		}
		o->tag_x[i] = x;
		text(img, x + PAD, 0, h, w, label, fg);
		x += cw;
	}
	o->tag_x[NTAGS] = x;

	/* od prawej: motyw, zegar */
	char clock[32];
	time_t now = time(NULL);
	strftime(clock, sizeof clock, "%a %d.%m %H:%M", localtime(&now));

	char label[32];
	snprintf(label, sizeof label, "[ %s ]", t->label);
	int lw = text_width(label) + 2 * PAD;
	o->theme_x = w - lw;
	text(img, o->theme_x + PAD, 0, h, w, label, t->accent);

	int cx = o->theme_x - text_width(clock) - PAD;
	text(img, cx, 0, h, o->theme_x, clock, t->fg);

	/* tytuł okna z fokusem — tylko na jego monitorze */
	if (focused && focused->out == o && focused->tag == o->tag && focused->title)
		text(img, x + 2 * PAD, 0, h, cx - 2 * PAD, focused->title, t->fg);
}

/* Prosty, powtarzalny generator: te same gwiazdy po każdym przerysowaniu. */
static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

static void draw_wall(struct output *o, pixman_image_t *img, int w, int h)
{
	const struct theme *t = THEME;
	rect(img, 0, 0, w, h, t->wall);

	if (theme_idx == 0) {               /* GRUVBOX: siatka kropek */
		for (int y = 12; y < h; y += 24)
			for (int x = 12; x < w; x += 24)
				rect(img, x, y, 2, 2, t->dot);
	} else {                            /* MU/TH/UR: gwiazdy */
		uint32_t seed = (uint32_t)(o->x * 7919 + o->y * 104729 + w);
		int n = w * h / 5000;
		for (int i = 0; i < n; i++) {
			int x = rnd(&seed) % w, y = rnd(&seed) % h;
			uint32_t r = rnd(&seed) % 100;
			uint32_t c = r < 70 ? t->line : r < 95 ? t->dim : t->fg;
			int sz = r < 97 ? 1 : 2;
			rect(img, x, y, sz, sz, c);
		}
	}

	const char *mark = theme_idx == 0 ? "norns" : "MU/TH/UR 6000";
	int mw = text_width(mark);
	int mh = fonts[theme_idx]->height;
	text(img, w - mw - 24, h - mh - 24, mh, w, mark, theme_idx == 0 ? t->focus : t->dim);
}

/* ---------- powierzchnie ---------- */

static void surf_init(struct surf *s, struct output *o)
{
	s->wl = wl_compositor_create_surface(compositor);
	wl_surface_set_user_data(s->wl, o);
	s->ss = river_window_manager_v1_get_shell_surface(wm, s->wl);
	s->node = river_shell_surface_v1_get_node(s->ss);
	s->dirty = true;
}

static void surf_fini(struct surf *s)
{
	if (ptr_surf == s->wl)
		ptr_surf = NULL;
	river_node_v1_destroy(s->node);
	river_shell_surface_v1_destroy(s->ss);
	wl_surface_destroy(s->wl);
}

void urd_output_init(struct output *o)
{
	surf_init(&o->bar, o);
	surf_init(&o->wall, o);
}

void urd_output_fini(struct output *o)
{
	surf_fini(&o->bar);
	surf_fini(&o->wall);
}

/* Rysuje i commituje powierzchnię zsynchronizowaną z render_finish. */
static void surf_draw(struct surf *s, struct output *o,
                      void (*draw)(struct output *, pixman_image_t *, int, int))
{
	if (s->w <= 0 || s->h <= 0)
		return;
	if (!s->dirty && s->w == s->drawn_w && s->h == s->drawn_h)
		return;
	struct buf *b = buf_new(s->w, s->h);
	if (!b)
		return;
	draw(o, b->img, s->w, s->h);
	river_shell_surface_v1_sync_next_commit(s->ss);
	wl_surface_attach(s->wl, b->wb, 0, 0);
	wl_surface_damage_buffer(s->wl, 0, 0, s->w, s->h);
	wl_surface_commit(s->wl);
	s->drawn_w = s->w;
	s->drawn_h = s->h;
	s->dirty = false;
}

void urd_render(struct output *o)
{
	surf_draw(&o->wall, o, draw_wall);
	surf_draw(&o->bar, o, draw_bar);
	river_node_v1_set_position(o->wall.node, o->wall.x, o->wall.y);
	river_node_v1_set_position(o->bar.node, o->bar.x, o->bar.y);
	river_node_v1_place_bottom(o->wall.node);   /* tapeta pod wszystkim */
	river_node_v1_place_top(o->bar.node);       /* pasek nad wszystkim */
}

/* ---------- mysz na pasku ---------- */

static void ptr_enter(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s, wl_fixed_t x, wl_fixed_t y)
{
	ptr_surf = s;
	ptr_x = wl_fixed_to_int(x);
	if (cursor_img)
		wl_pointer_set_cursor(p, serial, cursor_surf, cursor_img->hotspot_x, cursor_img->hotspot_y);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	ptr_surf = NULL;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_int(x);
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	if (state != WL_POINTER_BUTTON_STATE_PRESSED || button != BTN_LEFT || !ptr_surf)
		return;
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		if (o->bar.wl != ptr_surf)
			continue;
		if (ptr_x >= o->theme_x) {
			toggle_theme();
			return;
		}
		for (int i = 0; i < NTAGS; i++)
			if (ptr_x >= o->tag_x[i] && ptr_x < o->tag_x[i + 1])
				view_tag(o, i);
		return;
	}
}

static void ptr_axis(void *d, struct wl_pointer *p, uint32_t time, uint32_t axis, wl_fixed_t v) {}

static const struct wl_pointer_listener ptr_listener = {
	.enter = ptr_enter,
	.leave = ptr_leave,
	.motion = ptr_motion,
	.button = ptr_button,
	.axis = ptr_axis,
};

static void seat_caps(void *d, struct wl_seat *seat, uint32_t caps)
{
	if ((caps & WL_SEAT_CAPABILITY_POINTER) && !pointer) {
		pointer = wl_seat_get_pointer(seat);
		wl_pointer_add_listener(pointer, &ptr_listener, NULL);
	} else if (!(caps & WL_SEAT_CAPABILITY_POINTER) && pointer) {
		wl_pointer_destroy(pointer);
		pointer = NULL;
	}
}

static const struct wl_seat_listener seat_listener = { .capabilities = seat_caps };

void urd_bind_seat(struct wl_registry *reg, uint32_t name)
{
	struct wl_seat *seat = wl_registry_bind(reg, name, &wl_seat_interface, 1);
	wl_seat_add_listener(seat, &seat_listener, NULL);
}

/* ---------- start ---------- */

bool urd_init(void)
{
	fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR);
	for (int i = 0; i < 2; i++) {
		const char *names[] = { themes[i].font, "monospace:pixelsize=14" };
		fonts[i] = fcft_from_name(2, names, NULL);
		if (!fonts[i]) {
			fprintf(stderr, "urd: nie mogę załadować fontu %s\n", themes[i].font);
			return false;
		}
	}

	const char *size = getenv("XCURSOR_SIZE");
	struct wl_cursor_theme *ct = wl_cursor_theme_load(getenv("XCURSOR_THEME"), size ? atoi(size) : 24, shm);
	struct wl_cursor *c = ct ? wl_cursor_theme_get_cursor(ct, "default") : NULL;
	if (ct && !c)
		c = wl_cursor_theme_get_cursor(ct, "left_ptr");
	if (c) {
		cursor_img = c->images[0];
		cursor_surf = wl_compositor_create_surface(compositor);
		wl_surface_attach(cursor_surf, wl_cursor_image_get_buffer(cursor_img), 0, 0);
		wl_surface_commit(cursor_surf);
	}
	return true;
}
