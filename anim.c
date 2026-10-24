// anim — ślizg tagów, płynny ruch okien i otwieranie w stylu kineskopu
//
// River pozwala WM-owi ustawiać pozycje i przycięcie okien w każdej sekwencji
// render. Animujemy więc tylko to, co nie zmusza aplikacji do przerysowania
// się: pozycję i przycięcie. Rozmiar zmienia się skokowo.
#define _POSIX_C_SOURCE 200809L
#include <time.h>
#include "norns.h"

#define SLIDE_MS 140
#define MOVE_MS  120
#define OPEN_MS  160

bool anim_on = true;

uint64_t now_ms(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static double progress(uint64_t t0, uint64_t now, int ms)
{
	if (!t0 || now >= t0 + ms)
		return 1.0;
	return (double)(now - t0) / ms;
}

/* szybki start, miękkie hamowanie */
static double ease(double p)
{
	double q = 1.0 - p;
	return 1.0 - q * q * q;
}

static int lerp(int a, int b, double e)
{
	return a + (int)((b - a) * e);
}

static bool sliding(struct output *o, uint64_t now)
{
	if (o->slide_t0 && now >= o->slide_t0 + SLIDE_MS)
		o->slide_t0 = 0;
	return o->slide_t0 != 0;
}

void anim_tag_switch(struct output *o, int from, int to)
{
	if (!anim_on)
		return;
	o->slide_from = from;
	o->slide_dir = to > from ? 1 : -1;  /* wyższy tag wjeżdża z prawej */
	o->slide_t0 = now_ms();
}

bool anim_busy(uint64_t now)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		if (sliding(o, now))
			return true;
	struct win *w;
	wl_list_for_each(w, &windows, link)
		if ((w->move_t0 && now < w->move_t0 + MOVE_MS) || (w->open_t0 && now < w->open_t0 + OPEN_MS))
			return true;
	return false;
}

/* Część wspólna prostokąta a i b; false, jeśli pusta. */
static bool intersect(int *x, int *y, int *w, int *h, int bx, int by, int bw, int bh)
{
	int x1 = *x > bx ? *x : bx, y1 = *y > by ? *y : by;
	int x2 = *x + *w < bx + bw ? *x + *w : bx + bw;
	int y2 = *y + *h < by + bh ? *y + *h : by + bh;
	*x = x1;
	*y = y1;
	*w = x2 - x1;
	*h = y2 - y1;
	return *w > 0 && *h > 0;
}

void anim_render_window(struct win *w, uint64_t now)
{
	struct output *o = w->out;
	bool vis = win_visible(w);
	bool slide = o && sliding(o, now);
	bool leaving = !vis && slide && w->tag == o->slide_from;

	if (!vis && !leaving) {
		river_window_v1_hide(w->rw);
		w->shown = false;
		return;
	}

	/* pozycja bazowa: cel z layoutu, ewentualnie w trakcie dojazdu */
	if (vis) {
		if (!w->shown) {                /* pojawia się: bez dojazdu */
			w->bx = w->x;
			w->by = w->y;
			w->move_t0 = 0;
		} else if (w->x != w->tx || w->y != w->ty) {
			w->mfx = w->bx;
			w->mfy = w->by;
			w->move_t0 = anim_on ? now : 0;
		}
		w->tx = w->x;
		w->ty = w->y;
		if (!w->ever_shown && w->dw > 0) {  /* pierwsza klatka z treścią: otwieranie */
			w->ever_shown = true;
			w->open_t0 = anim_on ? now : 0;
		}

		double p = progress(w->move_t0, now, MOVE_MS);
		if (p >= 1.0) {
			w->move_t0 = 0;
			w->bx = w->x;
			w->by = w->y;
		} else {
			w->bx = lerp(w->mfx, w->x, ease(p));
			w->by = lerp(w->mfy, w->y, ease(p));
		}
	}
	w->shown = true;

	/* ślizg tagów: nowy wjeżdża, stary wyjeżdża w przeciwną stronę */
	int off = 0;
	if (slide) {
		double e = ease(progress(o->slide_t0, now, SLIDE_MS));
		int d = o->slide_dir * o->w;
		off = vis ? (int)((1.0 - e) * d) : (int)(-e * d);
	}
	int px = w->bx + off, py = w->by;

	/* przycięcie: okno z ramką i poświatą, w trakcie ślizgu do monitora,
	 * przy otwieraniu rozwijane od środkowej linii jak włączany kineskop */
	int ww = w->dw > 0 ? w->dw : w->w, wh = w->dh > 0 ? w->dh : w->h;
	int m = BORDER + w->deco_r;
	int fx = px - m, fy = py - m, fw = ww + 2 * m, fh = wh + 2 * m;
	int cx = fx, cy = fy, cw = fw, ch = fh;
	bool ok = true;

	double op = progress(w->open_t0, now, OPEN_MS);
	if (op < 1.0) {
		double e = ease(op);
		int nw = (int)(fw * (0.7 + 0.3 * e)), nh = (int)(fh * e);
		if (nh < 2)
			nh = 2;
		cx = fx + (fw - nw) / 2;
		cy = fy + (fh - nh) / 2;
		cw = nw;
		ch = nh;
	} else {
		w->open_t0 = 0;
	}
	if (slide)
		ok = intersect(&cx, &cy, &cw, &ch, o->x, o->y, o->w, o->h);

	if (!ok) {                          /* całkiem poza monitorem */
		river_window_v1_hide(w->rw);
		return;
	}
	river_window_v1_show(w->rw);
	river_node_v1_set_position(w->rn, px, py);

	bool full = cx == fx && cy == fy && cw == fw && ch == fh;
	if (!full) {
		river_window_v1_set_clip_box(w->rw, cx - px, cy - py, cw, ch);
		w->clip_on = true;
	} else if (w->clip_on) {
		river_window_v1_set_clip_box(w->rw, 0, 0, 0, 0);   /* 0x0 = bez przycinania */
		w->clip_on = false;
	}
}
