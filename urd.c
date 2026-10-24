// urd — pasek i tapeta norns, rysowane do buforów wl_shm (pixman + fcft)
#define _GNU_SOURCE
#include <linux/input-event-codes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
static struct wl_cursor_theme *cursor_theme;
static int ptr_x;

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

/* Pixman przy jednolitym wypełnieniu NIE przycina do obrazka — robimy to sami. */
static void rect(pixman_image_t *img, int x, int y, int w, int h, uint32_t rgb)
{
	int iw = pixman_image_get_width(img), ih = pixman_image_get_height(img);
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > iw) w = iw - x;
	if (y + h > ih) h = ih - y;
	if (w <= 0 || h <= 0)
		return;
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

/* Sensory od prawej do lewej, kończąc przed `right`. Zwraca x początku. */
static int draw_sensors(pixman_image_t *img, int right, int h, int min_x)
{
	const struct theme *t = THEME;
	struct { char s[24]; uint32_t c; } it[8];
	int n = 0;

	if (sens.wg) {
		snprintf(it[n].s, sizeof it[n].s, "WG");
		it[n++].c = t->accent;
	}
	snprintf(it[n].s, sizeof it[n].s, "%s", sens.lan ? "LAN" : sens.wifi ? "WIFI" : "OFFLINE");
	it[n++].c = sens.lan || sens.wifi ? t->fg : t->alarm;

	snprintf(it[n].s, sizeof it[n].s, "CPU %d%%", sens.cpu);
	it[n++].c = sens.cpu >= 90 ? t->alarm : sens.cpu >= 70 ? t->warn : t->fg;

	snprintf(it[n].s, sizeof it[n].s, "RAM %.1fG", sens.ram_gb);
	it[n++].c = t->fg;

	if (sens.has_temp) {
		snprintf(it[n].s, sizeof it[n].s, "%d°C", sens.temp);
		it[n++].c = sens.temp >= 90 ? t->alarm : sens.temp >= 80 ? t->warn : t->fg;
	}
	if (sens.has_bat) {
		snprintf(it[n].s, sizeof it[n].s, "BAT %d%%%c", sens.bat, sens.bat_state);
		bool low = sens.bat_state == '-';
		it[n++].c = low && sens.bat <= 10 ? t->alarm : low && sens.bat <= 20 ? t->warn : t->fg;
	}

	int x = right;
	for (int i = n - 1; i >= 0; i--) {
		int w = text_width(it[i].s);
		if (x - w < min_x)
			break;
		x -= w;
		text(img, x, 0, h, x + w, it[i].s, it[i].c);
		x -= 2 * PAD;
	}
	return x + 2 * PAD;
}

/*  [1]…[9]  tytuł okna ......  WG LAN CPU RAM °C BAT  Fri 23.10 21:41  [ GRUVBOX ] */
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

	/* od prawej: motyw, zegar, sensory */
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

	int sx = draw_sensors(img, cx - 2 * PAD, h, x);

	/* tytuł okna z fokusem — tylko na jego monitorze */
	if (focused && focused->out == o && focused->tag == o->tag && focused->title)
		text(img, x + 2 * PAD, 0, h, sx - 2 * PAD, focused->title, t->fg);
}

/* ---------- tapeta: GRUVBOX kropki, MU/TH/UR żywe gwiazdy i kometa ---------- */

#define TRAIL       28              /* punkty ogona komety */
#define TWINKLE_MS  200             /* tempo migotania */
#define COMET_MS    33              /* klatki w czasie przelotu komety */
#define COMET_LIFE  1400

struct star {
	int x, y, size;
	int base;                       /* 0 line, 1 dim, 2 fg */
	bool twinkle;
	float phase, period;            /* period w ms */
};

struct sky {
	int w, h, theme;
	struct buf *b[2];
	bool stale[2];                  /* bufor wymaga pełnego przerysowania */
	int ntrail[2];
	int trail[2][TRAIL][2];         /* piksele komety narysowane w danym buforze */
	int last;                       /* ostatnio wysłany bufor */
	struct star *stars;
	int nstars;
	int mark_x, mark_y, mark_w, mark_h;
	uint64_t comet_t0, next_comet;
	float cx, cy, vx, vy;           /* start i prędkość (px/ms) */
	uint32_t seed;
	uint64_t last_frame;
};

/* Prosty, powtarzalny generator. */
static uint32_t rnd(uint32_t *s)
{
	*s = *s * 1103515245u + 12345u;
	return *s >> 8;
}

static void sky_free_bufs(struct sky *k)
{
	for (int i = 0; i < 2; i++) {
		buf_drop(k->b[i]);
		k->b[i] = NULL;
	}
}

/* Gwiazdy losowane raz na rozmiar monitora — przerysowanie daje ten sam układ. */
static void sky_setup(struct output *o, struct sky *k, int w, int h)
{
	sky_free_bufs(k);
	free(k->stars);
	k->w = w;
	k->h = h;
	k->theme = -1;
	uint32_t seed = (uint32_t)(o->x * 7919 + o->y * 104729 + w);
	k->nstars = w * h / 5000;
	k->stars = calloc(k->nstars, sizeof *k->stars);
	for (int i = 0; i < k->nstars; i++) {
		struct star *st = &k->stars[i];
		st->x = rnd(&seed) % w;
		st->y = rnd(&seed) % h;
		uint32_t r = rnd(&seed) % 100;
		st->base = r < 70 ? 0 : r < 95 ? 1 : 2;
		st->size = r < 97 ? 1 : 2;
		st->twinkle = rnd(&seed) % 100 < 15;
		st->phase = (rnd(&seed) % 1000) / 1000.0f * 6.2832f;
		st->period = 2500 + rnd(&seed) % 5500;
	}
	k->seed = seed;
}

static uint32_t star_color(const struct star *st, uint64_t now)
{
	const struct theme *t = THEME;
	const uint32_t pal[4] = { t->line, t->dim, t->fg, t->accent };
	int idx = st->base;
	if (st->twinkle && anim_on) {
		uint64_t per = (uint64_t)st->period;
		float s = sinf(st->phase + 6.2832f * (float)(now % per) / st->period);
		idx += s > 0.6f ? 1 : s < -0.6f ? -1 : 0;
	}
	return pal[idx < 0 ? 0 : idx > 3 ? 3 : idx];
}

static void sky_mark(struct sky *k, pixman_image_t *img)
{
	const struct theme *t = THEME;
	const char *mark = theme_idx == 0 ? "norns" : "MU/TH/UR 6000";
	k->mark_w = text_width(mark);
	k->mark_h = fonts[theme_idx]->height;
	k->mark_x = k->w - k->mark_w - 24;
	k->mark_y = k->h - k->mark_h - 24;
	rect(img, k->mark_x, k->mark_y, k->mark_w, k->mark_h, t->wall);
	text(img, k->mark_x, k->mark_y, k->mark_h, k->w, mark, theme_idx == 0 ? t->focus : t->dim);
}

/* Głowa i ogon komety w chwili now: jasność maleje wzdłuż ogona. */
static int comet_points(struct sky *k, uint64_t now, int pts[TRAIL][2], uint32_t col[TRAIL])
{
	const struct theme *t = THEME;
	if (!k->comet_t0)
		return 0;
	float dt = (float)(now - k->comet_t0);
	int n = 0;
	for (int i = 0; i < TRAIL; i++) {
		float tt = dt - i * 2.5f;           /* każdy punkt ogona 2,5 ms wcześniej */
		if (tt < 0)
			break;
		int x = (int)(k->cx + k->vx * tt), y = (int)(k->cy + k->vy * tt);
		if (x < 0 || y < 0 || x >= k->w || y >= k->h)
			continue;
		pts[n][0] = x;
		pts[n][1] = y;
		col[n] = i < 4 ? t->accent : i < 10 ? t->fg : i < 18 ? t->dim : t->line;
		n++;
	}
	return n;
}

static void sky_full(struct output *o, struct sky *k, pixman_image_t *img, uint64_t now)
{
	const struct theme *t = THEME;
	rect(img, 0, 0, k->w, k->h, t->wall);
	if (theme_idx == 0) {
		for (int y = 12; y < k->h; y += 24)
			for (int x = 12; x < k->w; x += 24)
				rect(img, x, y, 2, 2, t->dot);
		sky_mark(k, img);
		return;
	}
	sky_mark(k, img);
	for (int i = 0; i < k->nstars; i++) {
		struct star *st = &k->stars[i];
		rect(img, st->x, st->y, st->size, st->size, star_color(st, now));
	}
}

/* Wybiera wolny bufor (najlepiej nie ten, który compositor właśnie pokazuje). */
static int sky_pick(struct sky *k)
{
	int order[2] = { !k->last, k->last };
	for (int j = 0; j < 2; j++) {
		int i = order[j];
		if (!k->b[i]) {
			k->b[i] = buf_new(k->w, k->h, false, true);
			k->stale[i] = true;
		}
		if (k->b[i] && !k->b[i]->busy)
			return i;
	}
	return -1;
}

/* Pełna tapeta przy zmianie rozmiaru lub motywu — wołane z sekwencji render. */
static void sky_render(struct output *o, bool sync)
{
	struct surf *s = &o->wall;
	if (s->w <= 0 || s->h <= 0)
		return;
	if (!o->sky)
		o->sky = calloc(1, sizeof *o->sky);
	struct sky *k = o->sky;
	if (k->w != s->w || k->h != s->h)
		sky_setup(o, k, s->w, s->h);
	if (!s->dirty && k->theme == theme_idx)
		return;
	k->stale[0] = k->stale[1] = true;
	k->ntrail[0] = k->ntrail[1] = 0;
	int i = sky_pick(k);
	if (i < 0)
		return;                     /* oba zajęte — spróbujemy w następnej sekwencji */
	uint64_t now = now_ms();
	sky_full(o, k, k->b[i]->img, now);
	k->stale[i] = false;
	k->theme = theme_idx;
	if (sync)
		river_shell_surface_v1_sync_next_commit(s->ss);
	buf_attach(k->b[i], s->wl);
	wl_surface_damage_buffer(s->wl, 0, 0, k->w, k->h);
	wl_surface_commit(s->wl);
	k->last = i;
	s->dirty = false;
}

/* Jedna klatka żywej tapety: tylko zmienione piksele, commit bez sync. */
static int sky_frame(struct output *o, struct sky *k, uint64_t now)
{
	struct surf *s = &o->wall;

	if (!k->next_comet)
		k->next_comet = now + 8000 + rnd(&k->seed) % 30000;
	if (!k->comet_t0 && now >= k->next_comet) {   /* nowa kometa: z góry, w dół i w bok */
		k->comet_t0 = now;
		k->cx = rnd(&k->seed) % k->w;
		k->cy = 0;
		float ang = (25 + rnd(&k->seed) % 30) * 3.14159f / 180;
		float dir = rnd(&k->seed) % 2 ? 1.0f : -1.0f;
		float speed = (k->h * 0.9f) / COMET_LIFE / sinf(ang);
		k->vx = dir * speed * cosf(ang);
		k->vy = speed * sinf(ang);
	}
	bool comet = k->comet_t0 && now < k->comet_t0 + COMET_LIFE + TRAIL * 3;
	if (k->comet_t0 && !comet) {
		k->comet_t0 = 0;
		k->next_comet = now + 25000 + rnd(&k->seed) % 45000;
	}

	if (!comet && k->ntrail[0] == 0 && k->ntrail[1] == 0 && now < k->last_frame + TWINKLE_MS)
		return TWINKLE_MS;

	int i = sky_pick(k);
	if (i < 0)
		return COMET_MS;
	pixman_image_t *img = k->b[i]->img;
	const struct theme *t = THEME;

	if (k->stale[i]) {
		sky_full(o, k, img, now);
		k->stale[i] = false;
		wl_surface_damage_buffer(s->wl, 0, 0, k->w, k->h);
	} else {
		for (int j = 0; j < k->ntrail[i]; j++) {        /* zmaż starą kometę z tego bufora */
			rect(img, k->trail[i][j][0], k->trail[i][j][1], 2, 2, t->wall);
			wl_surface_damage_buffer(s->wl, k->trail[i][j][0], k->trail[i][j][1], 2, 2);
		}
		if (k->ntrail[i])
			sky_mark(k, img);
		for (int j = 0; j < k->nstars; j++) {
			struct star *st = &k->stars[j];
			rect(img, st->x, st->y, st->size, st->size, star_color(st, now));
			if (st->twinkle)
				wl_surface_damage_buffer(s->wl, st->x, st->y, st->size, st->size);
		}
	}

	int pts[TRAIL][2];
	uint32_t col[TRAIL];
	int n = comet_points(k, now, pts, col);
	for (int j = n - 1; j >= 0; j--) {                  /* ogon pod głową */
		rect(img, pts[j][0], pts[j][1], 2, 2, col[j]);
		wl_surface_damage_buffer(s->wl, pts[j][0], pts[j][1], 2, 2);
		k->trail[i][j][0] = pts[j][0];
		k->trail[i][j][1] = pts[j][1];
	}
	k->ntrail[i] = n;

	buf_attach(k->b[i], s->wl);
	wl_surface_commit(s->wl);
	k->last = i;
	k->last_frame = now;
	return comet ? COMET_MS : TWINKLE_MS;
}

int urd_sky_frame(uint64_t now)
{
	if (theme_idx != 1 || !anim_on)
		return -1;
	int next = TWINKLE_MS;
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		struct sky *k = o->sky;
		if (!k || !k->w || k->theme != theme_idx)
			continue;
		int n = sky_frame(o, k, now);
		if (n < next)
			next = n;
	}
	return next;
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
	if (o->sky) {
		sky_free_bufs(o->sky);
		free(o->sky->stars);
		free(o->sky);
	}
}

/* Rysuje i commituje powierzchnię; sync = razem z najbliższym render_finish. */
static void surf_draw(struct surf *s, struct output *o, bool sync,
                      void (*draw)(struct output *, pixman_image_t *, int, int))
{
	if (s->w <= 0 || s->h <= 0)
		return;
	if (!s->dirty && s->w == s->drawn_w && s->h == s->drawn_h)
		return;
	struct buf *b = buf_new(s->w, s->h, false, false);
	if (!b)
		return;
	draw(o, b->img, s->w, s->h);
	if (sync)
		river_shell_surface_v1_sync_next_commit(s->ss);
	buf_attach(b, s->wl);
	wl_surface_damage_buffer(s->wl, 0, 0, s->w, s->h);
	wl_surface_commit(s->wl);
	s->drawn_w = s->w;
	s->drawn_h = s->h;
	s->dirty = false;
}

void urd_render(struct output *o)
{
	sky_render(o, true);
	surf_draw(&o->bar, o, true, draw_bar);
	river_node_v1_set_position(o->wall.node, o->wall.x, o->wall.y);
	river_node_v1_set_position(o->bar.node, o->bar.x, o->bar.y);
	river_node_v1_place_bottom(o->wall.node);   /* tapeta pod wszystkim */
	river_node_v1_place_top(o->bar.node);       /* pasek nad wszystkim */
}

/* Zegar i sensory zmieniają się bez udziału okien — commit od razu, bez sync. */
void urd_redraw_bars(void)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		o->bar.dirty = true;
		surf_draw(&o->bar, o, false, draw_bar);
	}
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

/* Od wersji 4 protokołu kursor ustawiony przez WM obowiązuje, gdy żaden klient
 * nie ma wskaźnika (np. przy przeciąganiu), a serial jest ignorowany. */
void urd_cursor(const char *name)
{
	if (!pointer || !cursor_theme)
		return;
	struct wl_cursor *c = wl_cursor_theme_get_cursor(cursor_theme, name);
	if (!c)
		c = wl_cursor_theme_get_cursor(cursor_theme, "default");
	if (!c)
		return;
	cursor_img = c->images[0];
	wl_surface_attach(cursor_surf, wl_cursor_image_get_buffer(cursor_img), 0, 0);
	wl_surface_damage_buffer(cursor_surf, 0, 0, cursor_img->width, cursor_img->height);
	wl_surface_commit(cursor_surf);
	wl_pointer_set_cursor(pointer, 0, cursor_surf, cursor_img->hotspot_x, cursor_img->hotspot_y);
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
	cursor_theme = ct;
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

#ifdef NORNS_TEST
void sky_test_comet(struct output *o, uint64_t now) { o->sky->next_comet = now; }
void sky_test_full(struct output *o, pixman_image_t *img, uint64_t now) { sky_full(o, o->sky, img, now); }
int sky_test_last(struct output *o) { return o->sky->last; }
uint64_t sky_test_lastframe(struct output *o) { return o->sky->last_frame; }
#endif
