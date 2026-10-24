// deco — poświata i cienie: dekoracja river pod oknem (get_decoration_below)
//
// GRUVBOX:  cień pod oknami pływającymi.
// MU/TH/UR: zielona poświata wokół okna z fokusem (jak świecący kineskop),
//           cień pod pływającymi bez fokusu.
// Kafelki mają tylko pół przerwy na poświatę, żeby nie wchodziła na sąsiadów.
#include <math.h>
#include "norns.h"

#define FLOAT_R 18

enum { DECO_NONE, DECO_SHADOW, DECO_GLOW };

void deco_init(struct win *w)
{
	for (int i = 0; i < 4; i++) {
		w->deco_wl[i] = wl_compositor_create_surface(compositor);
		w->deco[i] = river_window_v1_get_decoration_below(w->rw, w->deco_wl[i]);
	}
}

void deco_fini(struct win *w)
{
	for (int i = 0; i < 4; i++) {
		river_decoration_v1_destroy(w->deco[i]);
		wl_surface_destroy(w->deco_wl[i]);
	}
}

/* Miękkie wygaszanie w pasie r pikseli wokół okna z ramką (iw x ih).
 * Bufor b to wycinek całości zaczynający się w (sx, sy). */
static void draw_falloff(struct buf *b, int sx, int sy, int r, int iw, int ih, uint32_t rgb, double amax)
{
	uint32_t *px = b->data;
	int cr = (rgb >> 16) & 0xff, cg = (rgb >> 8) & 0xff, cb = rgb & 0xff;

	for (int y = 0; y < b->h; y++) {
		int vy = y + sy;
		int dy = vy < r ? r - vy : vy >= r + ih ? vy - (r + ih - 1) : 0;
		for (int x = 0; x < b->w; x++) {
			int vx = x + sx;
			int dx = vx < r ? r - vx : vx >= r + iw ? vx - (r + iw - 1) : 0;
			if (!dx && !dy)
				continue;               /* pod oknem — zostaje przezroczyste */
			double d = sqrt((double)dx * dx + (double)dy * dy);
			if (d >= r)
				continue;
			double f = 1.0 - d / r;
			int a = (int)(255 * amax * f * f);
			/* ARGB8888 w wl_shm: premultiplied alpha */
			px[y * b->w + x] = (uint32_t)a << 24 | (uint32_t)(cr * a / 255) << 16 |
			                   (uint32_t)(cg * a / 255) << 8 | (uint32_t)(cb * a / 255);
		}
	}
}

void deco_render(struct win *w)
{
	const struct theme *t = THEME;
	bool focus = w == focused;
	int kind = DECO_NONE, r = 0;
	uint32_t color = 0;
	double amax = 0;

	if (theme_idx == 1 && focus) {
		kind = DECO_GLOW;
		r = w->floating ? FLOAT_R : GAP / 2;
		color = t->focus;
		amax = 0.55;
	} else if (w->floating) {
		kind = DECO_SHADOW;
		r = FLOAT_R;
		color = 0x000000;
		amax = 0.5;
	}
	int ww = w->dw, wh = w->dh;
	if (ww <= 0 || wh <= 0)
		kind = DECO_NONE;
	if (kind == DECO_NONE) {
		r = 0;
		color = 0;
	}

	int iw = ww + 2 * BORDER, ih = wh + 2 * BORDER;    /* okno z ramką */
	int bw = iw + 2 * r, bh = ih + 2 * r;
	if (kind == w->deco_kind && r == w->deco_r && color == w->deco_color &&
	    (kind == DECO_NONE || (bw == w->deco_w && bh == w->deco_h)))
		return;

	/* cztery paski zamiast jednego bufora wielkości okna */
	const int strip[4][4] = {
		{ 0, 0, bw, r },                /* góra (z narożnikami) */
		{ 0, bh - r, bw, r },           /* dół */
		{ 0, r, r, ih },                /* lewo */
		{ bw - r, r, r, ih },           /* prawo */
	};
	for (int i = 0; i < 4; i++) {
		struct buf *b = NULL;
		if (kind != DECO_NONE) {
			b = buf_new(strip[i][2], strip[i][3], true, false);
			if (!b)
				continue;
			draw_falloff(b, strip[i][0], strip[i][1], r, iw, ih, color, amax);
		}
		river_decoration_v1_sync_next_commit(w->deco[i]);
		if (b) {
			buf_attach(b, w->deco_wl[i]);
			wl_surface_damage_buffer(w->deco_wl[i], 0, 0, b->w, b->h);
			river_decoration_v1_set_offset(w->deco[i], -BORDER - r + strip[i][0], -BORDER - r + strip[i][1]);
		} else {
			wl_surface_attach(w->deco_wl[i], NULL, 0, 0);
		}
		wl_surface_commit(w->deco_wl[i]);
	}

	w->deco_kind = kind;
	w->deco_r = r;
	w->deco_color = color;
	w->deco_w = bw;
	w->deco_h = bh;
}

#ifdef NORNS_TEST
void deco_test_falloff(struct buf *b, int r, uint32_t rgb, double amax) { draw_falloff(b, 0, 0, r, b->w - 2 * r, b->h - 2 * r, rgb, amax); }
#endif
#ifdef NORNS_TEST
int deco_test_strips(void)
{
	int r = 18, iw = 300, ih = 200, bw = iw + 2 * r, bh = ih + 2 * r, diff = 0;
	struct buf *full = buf_new(bw, bh, true, false);
	draw_falloff(full, 0, 0, r, iw, ih, 0x8cffb0, 0.55);
	const int strip[4][4] = { { 0, 0, bw, r }, { 0, bh - r, bw, r }, { 0, r, r, ih }, { bw - r, r, r, ih } };
	for (int i = 0; i < 4; i++) {
		struct buf *b = buf_new(strip[i][2], strip[i][3], true, false);
		draw_falloff(b, strip[i][0], strip[i][1], r, iw, ih, 0x8cffb0, 0.55);
		for (int y = 0; y < b->h; y++)
			for (int x = 0; x < b->w; x++)
				diff += ((uint32_t *)b->data)[y * b->w + x] != ((uint32_t *)full->data)[(y + strip[i][1]) * bw + x + strip[i][0]];
	}
	return diff;
}
#endif
