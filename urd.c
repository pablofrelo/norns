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

#define PAD_BASE 8                  /* odstępy poziome na pasku (logiczne px) */
static int PAD = PAD_BASE;          /* w pikselach bufora — przeskalowane przy rysowaniu */

static struct fcft_font *fonts[8];  /* po jednym na motyw, skala 1 */
static struct fcft_font *font;      /* font bieżącego rysowania (może być przeskalowany) */

/* Font motywu w danej skali: wzorzec z config.h z przeliczonym pixelsize. */
static struct {
	int theme, scale120;
	struct fcft_font *f;
} scaled[4];

static struct fcft_font *font_at(int scale120)
{
	if (scale120 == 120)
		return fonts[theme_idx];
	for (int i = 0; i < 4; i++)
		if (scaled[i].f && scaled[i].theme == theme_idx && scaled[i].scale120 == scale120)
			return scaled[i].f;
	char name[160];
	const char *pat = themes[theme_idx].font, *ps = strstr(pat, "pixelsize=");
	if (ps) {
		int px = atoi(ps + 10);
		snprintf(name, sizeof name, "%.*spixelsize=%d", (int)(ps - pat), pat, (px * scale120 + 60) / 120);
	} else {
		snprintf(name, sizeof name, "%s", pat);
	}
	const char *names[] = { name, "monospace" };
	struct fcft_font *f = fcft_from_name(2, names, NULL);
	if (!f)
		return fonts[theme_idx];
	static int next;
	if (scaled[next].f)
		fcft_destroy(scaled[next].f);
	scaled[next].theme = theme_idx;
	scaled[next].scale120 = scale120;
	scaled[next].f = f;
	next = (next + 1) % 4;
	return f;
}
static struct wl_pointer *pointer;
static struct wl_surface *ptr_surf, *cursor_surf;
static struct wl_cursor_image *cursor_img;
static struct wl_cursor_theme *cursor_theme;
static int ptr_x, ptr_y;

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
		const struct fcft_glyph *g = fcft_rasterize_char_utf32(font, next_cp(&s), FCFT_SUBPIXEL_NONE);
		if (g)
			w += g->advance.x;
	}
	return w;
}

/* Tekst wyśrodkowany w pionie w pasie [top, top+h); nie wychodzi za max_x. Zwraca x końca. */
static int text(pixman_image_t *img, int x, int top, int h, int max_x, const char *s, uint32_t rgb)
{
	struct fcft_font *f = font;
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
	struct { char s[24]; uint32_t c; } it[10];
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

	if (sens.vol >= 0) {
		if (sens.muted)
			snprintf(it[n].s, sizeof it[n].s, "VOL MUTE");
		else
			snprintf(it[n].s, sizeof it[n].s, "VOL %d%%", sens.vol);
		it[n++].c = sens.muted ? t->warn : t->fg;
	}
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

/* Ikonka w jednym kolorze motywu: jasność oryginału zostaje (szczegóły widać),
 * barwa znika. Ciemne fragmenty nie giną na ciemnym pasku — jasność od 35%. */
static pixman_image_t *tinted_icon(struct tray_item *it, uint32_t rgb)
{
	if (it->tinted && it->tint_src == it->icon && it->tint_rgb == rgb)
		return it->tinted;
	if (it->tinted) {
		void *d = pixman_image_get_data(it->tinted);
		pixman_image_unref(it->tinted);
		free(d);
		it->tinted = NULL;
	}
	int w = pixman_image_get_width(it->icon), h = pixman_image_get_height(it->icon);
	uint32_t *src = pixman_image_get_data(it->icon), *px = malloc((size_t)w * h * 4);
	if (!px)
		return it->icon;
	uint32_t cr = (rgb >> 16) & 0xff, cg = (rgb >> 8) & 0xff, cb = rgb & 0xff;
	for (int i = 0; i < w * h; i++) {
		uint32_t p = src[i], a = p >> 24;
		if (!a) {
			px[i] = 0;
			continue;
		}
		uint32_t r = (p >> 16) & 0xff, g = (p >> 8) & 0xff, b = p & 0xff;   /* premultiplied */
		float l = (0.30f * r + 0.59f * g + 0.11f * b) / a;                /* 0..1 bez alfy */
		float v = (0.35f + 0.65f * l) * a / 255.0f;
		px[i] = a << 24 | (uint32_t)(cr * v) << 16 | (uint32_t)(cg * v) << 8 | (uint32_t)(cb * v);
	}
	it->tinted = pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, px, w * 4);
	it->tint_src = it->icon;
	it->tint_rgb = rgb;
	return it->tinted;
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
		bool busy = o->roots[i] != NULL;          /* kafle albo pływające */
		struct win *wn;
		wl_list_for_each(wn, &windows, link)
			if (wn->out == o && wn->tag == i && !wn->hidden)
				busy = true;
		uint32_t fg = busy ? t->fg : t->dim;
		if (i == o->tag) {
			rect(img, x, 0, cw, h - 1, t->focus);
			fg = t->bg;
		} else if (busy) {                         /* zajęty: ramka dookoła numeru */
			int m = h / 8, lw = h / 22 > 1 ? h / 22 : 1;
			int fx = x + m / 2, fy = m, fw = cw - m, fh = h - 1 - 2 * m;
			rect(img, fx, fy, fw, lw, t->focus);
			rect(img, fx, fy + fh - lw, fw, lw, t->focus);
			rect(img, fx, fy, lw, fh, t->focus);
			rect(img, fx + fw - lw, fy, lw, fh, t->focus);
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

	/* zegar w odwróconych kolorach, jak aktywny tag — widać go od razu */
	int cw = text_width(clock) + 2 * PAD;
	int cx = o->theme_x - cw;
	rect(img, cx, 0, cw, h - 1, t->focus);
	text(img, cx + PAD, 0, h, o->theme_x, clock, t->bg);

	/* zasobnik: ikonki tuż przed zegarem */
	int slot = h, isz = h - 2 * (h / 8);
	o->tray_slot = slot;
	o->tray_x0 = cx - PAD - tray_n * slot;
	for (int i = 0; i < tray_n; i++) {
		pixman_image_t *ic = tray_items[i]->icon;
		if (ic && TRAY_TINT)
			ic = tinted_icon(tray_items[i], tray_items[i]->attention ? t->alarm : t->fg);
		int ix = o->tray_x0 + i * slot + (slot - isz) / 2, iy = (h - isz) / 2;
		if (ic) {
			int iw = pixman_image_get_width(ic), ih = pixman_image_get_height(ic);
			pixman_transform_t tf;
			pixman_transform_init_scale(&tf, pixman_double_to_fixed((double)iw / isz),
			                            pixman_double_to_fixed((double)ih / isz));
			pixman_image_set_transform(ic, &tf);
			pixman_image_set_filter(ic, PIXMAN_FILTER_BILINEAR, NULL, 0);
			pixman_image_composite32(PIXMAN_OP_OVER, ic, NULL, img, 0, 0, 0, 0, ix, iy, isz, isz);
			pixman_image_set_transform(ic, NULL);
		} else {
			rect(img, ix + isz / 4, iy + isz / 4, isz / 2, isz / 2, t->dim);   /* ikonka bez obrazka */
		}
		if (tray_items[i]->attention)
			rect(img, ix, h - 3, isz, 2, t->alarm);
	}
	int right = tray_n ? o->tray_x0 - PAD : cx - 2 * PAD;

	int sx = draw_sensors(img, right, h, x);

	/* tytuł okna z fokusem — tylko na jego monitorze */
	if (focused && focused->out == o && focused->tag == o->tag && focused->title)
		text(img, x + 2 * PAD, 0, h, sx - 2 * PAD, focused->title, t->fg);
}

/* ---------- tapeta: GRUVBOX kropki, MU/TH/UR żywe gwiazdy i kometa ---------- */

#define TRAIL       240             /* punkty ogona komety (co piksel) */
#define TWINKLE_MS  200             /* tempo migotania */
#define COMET_MS    16              /* klatki w czasie przelotu komety (~60/s) */
#define COMET_LIFE  2200            /* tyle trwa przelot przez wysokość ekranu */

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
	const char *mark = t->look == LOOK_PHOSPHOR ? "MU/TH/UR 6000" : "norns";
	k->mark_w = text_width(mark);
	k->mark_h = font->height;
	k->mark_x = k->w - k->mark_w - 24;
	k->mark_y = k->h - k->mark_h - 24;
	rect(img, k->mark_x, k->mark_y, k->mark_w, k->mark_h, t->wall);
	text(img, k->mark_x, k->mark_y, k->mark_h, k->w, mark, t->look == LOOK_DOTS ? t->focus : t->dim);
}

/* Głowa i ogon komety w chwili now: ciągła smuga co piksel wstecz od głowy,
 * jasność maleje wzdłuż ogona; punkty poza ekranem pomijane. */
static int comet_points(struct sky *k, uint64_t now, int pts[TRAIL][2], uint32_t col[TRAIL])
{
	const struct theme *t = THEME;
	if (!k->comet_t0)
		return 0;
	float dt = (float)(now - k->comet_t0);
	float hx = k->cx + k->vx * dt, hy = k->cy + k->vy * dt;
	float len = sqrtf(k->vx * k->vx + k->vy * k->vy);
	float ux = k->vx / len, uy = k->vy / len;
	float travelled = len * dt;
	int n = 0;
	for (int i = 0; i < TRAIL && i <= travelled; i++) {
		int x = (int)(hx - ux * i), y = (int)(hy - uy * i);
		if (x < 0 || y < 0 || x >= k->w - 1 || y >= k->h - 1)
			continue;
		pts[n][0] = x;
		pts[n][1] = y;
		float f = (float)i / TRAIL;
		col[n] = f < 0.06f ? t->accent : f < 0.25f ? t->fg : f < 0.6f ? t->dim : t->line;
		n++;
	}
	return n;
}

static void sky_full(struct output *o, struct sky *k, pixman_image_t *img, uint64_t now)
{
	const struct theme *t = THEME;
	rect(img, 0, 0, k->w, k->h, t->wall);
	if (t->look != LOOK_PHOSPHOR) {                          /* bez gwiazd */
		if (t->look == LOOK_DOTS)
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
	font = fonts[theme_idx];            /* tapeta zawsze w skali 1 */
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
		float speed = (float)k->h / COMET_LIFE / sinf(ang);
		k->vx = dir * speed * cosf(ang);
		k->vy = speed * sinf(ang);
	}
	/* leci, dopóki cały ogon nie zejdzie z ekranu (najwyżej 2× czas przelotu) */
	int pts[TRAIL][2];
	uint32_t col[TRAIL];
	int n = comet_points(k, now, pts, col);
	bool comet = k->comet_t0 && (n > 0 || now < k->comet_t0 + 200) && now < k->comet_t0 + 2 * COMET_LIFE;
	if (k->comet_t0 && !comet) {
		n = 0;
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

	int bx0 = k->w, by0 = k->h, bx1 = -1, by1 = -1;      /* obszar zmian komety */
	if (k->stale[i]) {
		sky_full(o, k, img, now);
		k->stale[i] = false;
		wl_surface_damage_buffer(s->wl, 0, 0, k->w, k->h);
	} else {
		for (int j = 0; j < k->ntrail[i]; j++) {        /* zmaż starą kometę z tego bufora */
			int x = k->trail[i][j][0], y = k->trail[i][j][1];
			rect(img, x, y, 2, 2, t->wall);
			if (x < bx0) bx0 = x;
			if (y < by0) by0 = y;
			if (x > bx1) bx1 = x;
			if (y > by1) by1 = y;
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

	for (int j = n - 1; j >= 0; j--) {                  /* ogon pod głową */
		int x = pts[j][0], y = pts[j][1];
		rect(img, x, y, 2, 2, col[j]);
		k->trail[i][j][0] = x;
		k->trail[i][j][1] = y;
		if (x < bx0) bx0 = x;
		if (y < by0) by0 = y;
		if (x > bx1) bx1 = x;
		if (y > by1) by1 = y;
	}
	k->ntrail[i] = n;
	if (bx1 >= 0)
		wl_surface_damage_buffer(s->wl, bx0, by0, bx1 - bx0 + 2, by1 - by0 + 2);

	buf_attach(k->b[i], s->wl);
	wl_surface_commit(s->wl);
	k->last = i;
	k->last_frame = now;
	return comet ? COMET_MS : TWINKLE_MS;
}

int urd_sky_frame(uint64_t now)
{
	font = fonts[theme_idx];
	if (THEME->look != LOOK_PHOSPHOR || !anim_on)
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

/* Compositor podaje skalę monitora, na którym jest pasek (np. 150 = 1,25). */
static void fs_preferred(void *data, struct wp_fractional_scale_v1 *fs, uint32_t scale)
{
	struct surf *s = data;
	if ((int)scale == s->scale120)
		return;
	s->scale120 = scale;
	s->dirty = true;
	river_window_manager_v1_manage_dirty(wm);   /* przerysuj w najbliższej sekwencji */
}

static const struct wp_fractional_scale_v1_listener fs_listener = { .preferred_scale = fs_preferred };

static void surf_init(struct surf *s, struct output *o, bool scaled)
{
	s->wl = wl_compositor_create_surface(compositor);
	wl_surface_set_user_data(s->wl, o);
	s->ss = river_window_manager_v1_get_shell_surface(wm, s->wl);
	s->node = river_shell_surface_v1_get_node(s->ss);
	s->dirty = true;
	s->scale120 = 120;
	if (scaled && viewporter && fscale_mgr) {
		s->vp = wp_viewporter_get_viewport(viewporter, s->wl);
		s->fs = wp_fractional_scale_manager_v1_get_fractional_scale(fscale_mgr, s->wl);
		wp_fractional_scale_v1_add_listener(s->fs, &fs_listener, s);
	}
}

static void surf_fini(struct surf *s)
{
	if (ptr_surf == s->wl)
		ptr_surf = NULL;
	if (s->fs)
		wp_fractional_scale_v1_destroy(s->fs);
	if (s->vp)
		wp_viewport_destroy(s->vp);
	river_node_v1_destroy(s->node);
	river_shell_surface_v1_destroy(s->ss);
	wl_surface_destroy(s->wl);
}

void urd_output_init(struct output *o)
{
	surf_init(&o->bar, o, true);           /* pasek ostry przy skali ułamkowej */
	surf_init(&o->wall, o, false);
	surf_init(&o->note, o, true);
	surf_init(&o->menu, o, true);
	o->note.alpha = true;                  /* przerwy między powiadomieniami przezroczyste */
	o->note.font_pct = NOTE_FONT;
}

void urd_output_fini(struct output *o)
{
	surf_fini(&o->bar);
	surf_fini(&o->wall);
	surf_fini(&o->note);
	surf_fini(&o->menu);
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
	int scale = s->vp ? s->scale120 : 120;
	if (!s->dirty && s->w == s->drawn_w && s->h == s->drawn_h && scale == s->drawn_scale)
		return;
	/* bufor w pikselach monitora, font i odstępy w tej samej skali */
	int bw = (s->w * scale + 60) / 120, bh = (s->h * scale + 60) / 120;
	struct buf *b = buf_new(bw, bh, s->alpha, false);
	if (!b)
		return;
	font = font_at(scale * (s->font_pct ? s->font_pct : 100) / 100);
	PAD = PAD_BASE * scale / 120;
	draw(o, b->img, bw, bh);
	font = fonts[theme_idx];
	PAD = PAD_BASE;
	if (sync)
		river_shell_surface_v1_sync_next_commit(s->ss);
	buf_attach(b, s->wl);
	if (s->vp)
		wp_viewport_set_destination(s->vp, s->w, s->h);   /* wyświetl w rozmiarze logicznym */
	wl_surface_damage_buffer(s->wl, 0, 0, bw, bh);
	wl_surface_commit(s->wl);
	s->drawn_w = s->w;
	s->drawn_h = s->h;
	s->drawn_scale = scale;
	s->dirty = false;
}

/* ---------- menu aplikacji z zasobnika ---------- */

static void menu_label(const struct tmenu_entry *e, char *out, size_t n)
{
	const char *mark = e->toggle == 1 ? (e->checked ? "[x] " : "[ ] ") :
	                   e->toggle == 2 ? (e->checked ? "(*) " : "( ) ") : "";
	snprintf(out, n, "%s%s%s", mark, e->label, e->submenu ? "  >" : "");
}

/* Szerokość i wysokość w pikselach bieżącego fontu (bufor albo skala 1). */
static void menu_size(int *w, int *h, int *rowh, int *sep)
{
	int lh = font->height;
	*rowh = lh + PAD / 2;
	*sep = PAD;
	int mw = 0, hh = PAD;
	char lab[160];
	for (int r = 0; r < tmenu.nrows; r++) {
		int i = tmenu.rows[r];
		if (i >= 0 && tmenu.e[i].separator) {
			hh += *sep;
			continue;
		}
		if (i < 0)
			snprintf(lab, sizeof lab, "< WSTECZ");
		else
			menu_label(&tmenu.e[i], lab, sizeof lab);
		int tw = text_width(lab);
		if (tw > mw)
			mw = tw;
		hh += *rowh;
	}
	*w = mw + 4 * PAD;
	if (*w < 12 * PAD)
		*w = 12 * PAD;
	*h = hh + PAD;
}

/* Wiersz pod kursorem (y logiczne w powierzchni menu). */
static int menu_row(struct output *o, int ly)
{
	int scale = o->menu.vp ? o->menu.scale120 : 120;
	int by = ly * scale / 120, y = o->menu_pad;
	for (int r = 0; r < tmenu.nrows; r++) {
		int i = tmenu.rows[r];
		int h = i >= 0 && tmenu.e[i].separator ? o->menu_pad : o->menu_rowh;
		if (by >= y && by < y + h)
			return i >= 0 && tmenu.e[i].separator ? -1 : r;
		y += h;
	}
	return -1;
}

static void draw_menu(struct output *o, pixman_image_t *img, int w, int h)
{
	const struct theme *t = THEME;
	int bw = w > 600 ? 3 : 2, rowh, sep, mw, mh;
	menu_size(&mw, &mh, &rowh, &sep);
	o->menu_rowh = rowh;
	o->menu_pad = PAD;
	rect(img, 0, 0, w, h, t->focus);
	rect(img, bw, bw, w - 2 * bw, h - 2 * bw, t->bg);
	int y = PAD;
	char lab[160];
	for (int r = 0; r < tmenu.nrows; r++) {
		int i = tmenu.rows[r];
		if (i >= 0 && tmenu.e[i].separator) {
			rect(img, 2 * PAD, y + sep / 2, w - 4 * PAD, 1, t->line);
			y += sep;
			continue;
		}
		bool on = r == tmenu.hover, dis = i >= 0 && !tmenu.e[i].enabled;
		if (on && !dis)
			rect(img, bw, y, w - 2 * bw, rowh, t->focus);
		if (i < 0)
			snprintf(lab, sizeof lab, "< WSTECZ");
		else
			menu_label(&tmenu.e[i], lab, sizeof lab);
		uint32_t c = dis ? t->dim : on ? t->bg : i < 0 ? t->dim : t->fg;
		text(img, 2 * PAD, y, rowh, w - PAD, lab, c);
		y += rowh;
	}
}

static void menu_render(struct output *o)
{
	struct surf *s = &o->menu;
	if (!tmenu.open || tmenu.out != o) {
		if (o->menu_shown) {
			river_shell_surface_v1_sync_next_commit(s->ss);
			wl_surface_attach(s->wl, NULL, 0, 0);
			wl_surface_commit(s->wl);
			o->menu_shown = false;
			s->drawn_w = s->drawn_h = 0;
		}
		return;
	}
	int scale = s->vp ? s->scale120 : 120, rowh, sep, bw, bh;
	font = font_at(scale);
	PAD = PAD_BASE * scale / 120;
	menu_size(&bw, &bh, &rowh, &sep);
	font = fonts[theme_idx];
	PAD = PAD_BASE;
	s->w = (bw * 120 + scale - 1) / scale;
	s->h = (bh * 120 + scale - 1) / scale;
	s->x = tmenu.x - s->w / 2;                                /* pod kursorem, nie za monitor */
	if (s->x + s->w > o->x + o->w - BORDER)
		s->x = o->x + o->w - BORDER - s->w;
	if (s->x < o->x + BORDER)
		s->x = o->x + BORDER;
	s->y = o->bar.y + o->bar.h;
	if (!o->menu_shown)
		s->dirty = true;
	surf_draw(s, o, true, draw_menu);
	river_node_v1_set_position(s->node, s->x, s->y);
	river_node_v1_place_top(s->node);
	o->menu_shown = true;
}

/* ---------- powiadomienia ---------- */

#define NL 7                        /* najwyżej linii treści w jednym powiadomieniu */
struct lines {
	char l[NL][200];
	uint32_t c[NL];
	int n;
};

/* Pierwsze `max` znaków (kodów Unicode) napisu; zwraca ile zostało z budżetu. */
static int take(char *dst, size_t n, const char *src, int max)
{
	const char *p = src;
	int k = 0;
	while (*p && k < max) {
		next_cp(&p);
		k++;
	}
	size_t len = (size_t)(p - src) < n - 1 ? (size_t)(p - src) : n - 1;
	memcpy(dst, src, len);
	dst[len] = 0;
	return max - k;
}

static void add_line(struct lines *L, const char *s, size_t len, uint32_t c, int maxl)
{
	if (L->n >= maxl || L->n >= NL)
		return;
	if (len >= sizeof L->l[0])
		len = sizeof L->l[0] - 1;
	memcpy(L->l[L->n], s, len);
	L->l[L->n][len] = 0;
	L->c[L->n++] = c;
}

/* Zawijanie po słowach do szerokości maxw (w bieżącym foncie), \n łamie linię. */
static void wrap(struct lines *L, const char *s, int maxw, uint32_t c, int maxl)
{
	char buf[200];
	while (*s && L->n < maxl) {
		const char *start = s, *last_fit = NULL;
		size_t blen = 0;
		while (*s && *s != '\n') {
			const char *q = s;
			next_cp(&q);
			size_t nl = blen + (q - s);
			if (nl >= sizeof buf)
				break;
			memcpy(buf + blen, s, q - s);
			buf[nl] = 0;
			if (text_width(buf) > maxw)
				break;
			blen = nl;
			if (*s == ' ')
				last_fit = s;
			s = q;
		}
		if (*s && *s != '\n' && last_fit && last_fit > start) {   /* tnij na ostatniej spacji */
			add_line(L, start, last_fit - start, c, maxl);
			s = last_fit + 1;
		} else {
			add_line(L, start, s - start, c, maxl);
			if (s == start && *s)                                /* znak szerszy niż pole */
				next_cp(&s);
		}
		if (*s == '\n')
			s++;
	}
}

/* Linie powiadomienia; visible < 0 = cały tekst (do liczenia wysokości). */
static void note_lines(const struct note *n, int maxw, int visible, struct lines *L)
{
	const struct theme *t = THEME;
	char part[512];
	L->n = 0;
	int left = visible < 0 ? 1 << 30 : visible;
	left = take(part, sizeof part, n->summary, left);
	wrap(L, part, maxw, n->urgency == 2 ? t->alarm : t->accent, 2);
	if (visible < 0 || left > 0) {
		take(part, sizeof part, n->body, left);
		wrap(L, part, maxw, t->fg, NL);
	}
}

static int note_height(const struct note *n, int w)
{
	struct lines L;
	note_lines(n, w - 4 * PAD, -1, &L);
	int lh = font->height;
	return 2 * PAD + lh * (1 + (L.n ? L.n : 1)) + PAD / 2;
}

static void draw_notes(struct output *o, pixman_image_t *img, int w, int h)
{
	const struct theme *t = THEME;
	uint64_t now = now_ms();
	int y = 0, bw = w > 600 ? 3 : 2, gap = GAP * PAD / PAD_BASE / 2;
	pixman_image_t *clear = img;
	pixman_color_t none = { 0 };
	pixman_rectangle16_t all = { 0, 0, w, h };
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, clear, &none, 1, &all);
	for (int i = 0; i < notes_n; i++) {
		const struct note *n = &notes[i];
		int nh = note_height(n, w);
		uint32_t edge = n->urgency == 2 ? t->alarm : t->focus;
		o->note_y[i] = y;
		rect(img, 0, y, w, nh, edge);
		rect(img, bw, y + bw, w - 2 * bw, nh - 2 * bw, t->bg);

		/* nagłówek: aplikacja wielkimi literami */
		char app[64];
		snprintf(app, sizeof app, "%s", *n->app ? n->app : "SYSTEM");
		for (char *p = app; *p; p++)
			if (*p >= 'a' && *p <= 'z')
				*p -= 32;
		int lh = font->height, x0 = 2 * PAD, ty = y + PAD;
		int ex = text(img, x0, ty, lh, w - 2 * PAD, "> ", t->dim);
		text(img, ex, ty, lh, w - 2 * PAD, app, t->dim);

		size_t len = note_chars(n);
		int tms = note_type_ms(len);
		int vis = now >= n->t0 + tms ? -1 : (int)((now - n->t0) * (len + 1) / (tms ? tms : 1));
		struct lines L;
		note_lines(n, w - 4 * PAD, vis, &L);
		int ly = ty + lh;
		for (int k = 0; k < L.n; k++, ly += lh) {
			int ex2 = text(img, x0, ly, lh, w - 2 * PAD, L.l[k], L.c[k]);
			if (vis >= 0 && k == L.n - 1)                     /* kursor przy pisaniu */
				rect(img, ex2 + 1, ly + lh / 6, lh / 2, lh * 2 / 3, t->fg);
		}
		if (vis >= 0 && L.n == 0)
			rect(img, x0, ly + lh / 6, lh / 2, lh * 2 / 3, t->fg);
		y += nh + gap;
	}
	o->note_y[notes_n] = y;
}

/* Wysokość całego stosu w logicznych px — liczona tym fontem i w tej skali,
 * w której powiadomienia będą narysowane, żeby nic nie ucięło dołu. */
static int notes_height(int scale)
{
	font = font_at(scale * NOTE_FONT / 100);
	PAD = PAD_BASE * scale / 120;
	int bw = (NOTE_W * scale + 60) / 120, gap = GAP * PAD / PAD_BASE / 2, y = 0;
	for (int i = 0; i < notes_n; i++)
		y += note_height(&notes[i], bw) + gap;
	if (y > 0)
		y -= gap;
	font = fonts[theme_idx];
	PAD = PAD_BASE;
	return (y * 120 + scale - 1) / scale;
}

static void notes_render(struct output *o)
{
	struct surf *s = &o->note;
	bool here = notes_n > 0 && !zen_on;                       /* na każdym monitorze; zen — czekają */
	if (!here) {
		if (o->note_shown) {                                  /* schowaj: pusty bufor */
			river_shell_surface_v1_sync_next_commit(s->ss);
			wl_surface_attach(s->wl, NULL, 0, 0);
			wl_surface_commit(s->wl);
			o->note_shown = false;
			s->drawn_w = s->drawn_h = 0;
		}
		return;
	}
	s->w = NOTE_W;
	s->h = notes_height(s->vp ? s->scale120 : 120);
	s->x = o->x + (o->w - NOTE_W) / 2;                        /* pośrodku u góry */
	s->y = o->bar.y + o->bar.h + GAP;
	if (notify_busy(now_ms()) || !o->note_shown)
		s->dirty = true;
	surf_draw(s, o, true, draw_notes);
	river_node_v1_set_position(s->node, s->x, s->y);
	river_node_v1_place_top(s->node);
	o->note_shown = true;
}

void urd_render(struct output *o)
{
	sky_render(o, true);
	surf_draw(&o->bar, o, true, draw_bar);
	river_node_v1_set_position(o->wall.node, o->wall.x, o->wall.y);
	river_node_v1_set_position(o->bar.node, o->bar.x, o->bar.y);
	river_node_v1_place_bottom(o->wall.node);   /* tapeta pod wszystkim */
	if (zen_on)
		river_node_v1_place_bottom(o->bar.node);    /* zen: pasek pod tapetą */
	else
		river_node_v1_place_top(o->bar.node);       /* pasek nad wszystkim */
	notes_render(o);
	menu_render(o);
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
	ptr_y = wl_fixed_to_int(y);
	if (cursor_img)
		wl_pointer_set_cursor(p, serial, cursor_surf, cursor_img->hotspot_x, cursor_img->hotspot_y);
}

static void ptr_leave(void *d, struct wl_pointer *p, uint32_t serial, struct wl_surface *s)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		if (!tmenu.open || tmenu.out != o)
			continue;
		if (o->menu.wl == s)
			tmenu_close();                                    /* kursor wyjechał z menu — zamykamy */
		else if (o->bar.wl == s) {                            /* z paska: zostaw tylko w dół, do menu */
			int gx = o->bar.x + ptr_x;
			bool down = ptr_y >= o->bar.h - 3 && gx >= o->menu.x && gx < o->menu.x + o->menu.w;
			if (!down)
				tmenu_close();
		}
	}
	ptr_surf = NULL;
}

static void ptr_motion(void *d, struct wl_pointer *p, uint32_t time, wl_fixed_t x, wl_fixed_t y)
{
	ptr_x = wl_fixed_to_int(x);
	ptr_y = wl_fixed_to_int(y);
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		if (o->menu.wl == ptr_surf && tmenu.open) {             /* podświetlenie wiersza */
			int r = menu_row(o, ptr_y);
			if (r != tmenu.hover) {
				tmenu.hover = r;
				o->menu.dirty = true;
				river_window_manager_v1_manage_dirty(wm);
			}
		}
}

static void ptr_button(void *d, struct wl_pointer *p, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
	if (state != WL_POINTER_BUTTON_STATE_PRESSED || !ptr_surf)
		return;
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		if (o->menu.wl == ptr_surf) {                         /* klik w menu z zasobnika */
			if (button == BTN_LEFT)
				tmenu_activate(menu_row(o, ptr_y));
			return;
		}
		if (o->note.wl == ptr_surf) {                         /* klik w powiadomienie */
			int by = ptr_y * (o->note.vp ? o->note.scale120 : 120) / 120;
			for (int i = 0; i < notes_n; i++)
				if (by >= o->note_y[i] && by < o->note_y[i + 1])
					notify_click(i);
			return;
		}
		if (o->bar.wl != ptr_surf)
			continue;
		/* granice na pasku są w pikselach bufora, kursor w logicznych */
		int bx = ptr_x * (o->bar.vp ? o->bar.scale120 : 120) / 120;
		if (tray_n && bx >= o->tray_x0 && bx < o->tray_x0 + tray_n * o->tray_slot) {
			int b = button == BTN_LEFT ? 1 : button == BTN_MIDDLE ? 2 : button == BTN_RIGHT ? 3 : 0;
			if (b)                                            /* menu pod paskiem, od kursora */
				tray_click((bx - o->tray_x0) / o->tray_slot, b, o->bar.x + ptr_x, o->bar.y + o->bar.h, o);
			return;
		}
		if (button != BTN_LEFT)
			return;
		if (bx >= o->theme_x) {
			toggle_theme();
			return;
		}
		for (int i = 0; i < NTAGS; i++)
			if (bx >= o->tag_x[i] && bx < o->tag_x[i + 1])
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
	for (int i = 0; i < nthemes && i < 8; i++) {
		const char *names[] = { themes[i].font, "monospace:pixelsize=14" };
		fonts[i] = fcft_from_name(2, names, NULL);
		if (!fonts[i]) {
			fprintf(stderr, "urd: nie mogę załadować fontu %s\n", themes[i].font);
			return false;
		}
	}

	font = fonts[theme_idx];
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
