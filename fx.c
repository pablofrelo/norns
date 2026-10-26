// fx — warstwa efektów nad całym ekranem: powierzchnia norns na samej górze,
// przepuszcza kliknięcia, rysowana w fizycznej rozdzielczości monitora.
// Na razie jeden efekt: linie kineskopu (Super+Ctrl+A, norns-ctl crt).
// Tu jest miejsce na kolejne — każdy dostaje własną powierzchnię w struct output
// i swoją funkcję wołaną z fx_render().
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "norns.h"

#define CRT_LINE     0.16f          /* przyciemnienie linii skanowania (0–1) */
#define CRT_VIGNETTE 0.35f          /* ściemnienie w samym rogu (0–1) */
#define CRT_CORNER   1.1f           /* od tej odległości (0 środek, 2 róg) zaczyna się ściemnianie */

bool crt_on;

struct fx {
	struct wl_surface *wl;
	struct river_shell_surface_v1 *ss;
	struct river_node_v1 *node;
	struct wp_viewport *vp;
	struct buf *b[2];
	int bw, bh;
};

static void fx_free(struct fx **slot)
{
	struct fx *f = *slot;
	if (!f)
		return;
	buf_drop(f->b[0]);
	buf_drop(f->b[1]);
	if (f->vp)
		wp_viewport_destroy(f->vp);
	river_node_v1_destroy(f->node);
	river_shell_surface_v1_destroy(f->ss);
	wl_surface_destroy(f->wl);
	free(f);
	*slot = NULL;
}

/* Przezroczysta powierzchnia nad wszystkim; bufor bw×bh rozciągany na cały monitor. */
static struct fx *fx_new(int bw, int bh)
{
	struct fx *f = calloc(1, sizeof *f);
	f->wl = wl_compositor_create_surface(compositor);
	struct wl_region *none = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(f->wl, none);                   /* kliknięcia przechodzą */
	wl_region_destroy(none);
	f->ss = river_window_manager_v1_get_shell_surface(wm, f->wl);
	f->node = river_shell_surface_v1_get_node(f->ss);
	if (viewporter)
		f->vp = wp_viewporter_get_viewport(viewporter, f->wl);
	f->bw = f->vp ? bw : 0;
	f->bh = f->vp ? bh : 0;
	return f;
}

/* Kineskop: co trzecia linia fizycznych pikseli przyciemniona, a w samych rogach
 * łagodne ściemnienie. Gradient rogu jest ditherowany (macierz Bayera 4×4), bo
 * 8-bitowa alfa bez tego układa się w widoczne pierścienie. */
static void draw_crt(struct buf *b)
{
	static const uint8_t bayer[4][4] = {
		{ 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 },
	};
	uint32_t *px = b->data;
	float cx = b->w / 2.0f, cy = b->h / 2.0f;
	for (int y = 0; y < b->h; y++) {
		float dy = (y - cy) / cy, line = y % 3 == 2 ? CRT_LINE : 0;
		for (int x = 0; x < b->w; x++) {
			float dx = (x - cx) / cx, r = dx * dx + dy * dy;      /* 0 środek, 2 róg */
			float vig = 0;
			if (r > CRT_CORNER) {                                 /* tylko rogi */
				float t = (r - CRT_CORNER) / (2 - CRT_CORNER);
				vig = t * t * CRT_VIGNETTE;
			}
			float a = 1 - (1 - line) * (1 - vig);
			int a8 = (int)(a * 255 + bayer[y & 3][x & 3] / 16.0f);
			px[y * b->w + x] = (uint32_t)(a8 > 255 ? 255 : a8) << 24;   /* czarny, premultiplied */
		}
	}
}

static bool fullscreen_on(struct output *o)
{
	struct win *w;
	wl_list_for_each(w, &windows, link)
		if (w->out == o && w->fullscreen && win_visible(w))
			return true;
	return false;
}

/* Warstwa kineskopu, dopóki jest włączona (bez pełnego ekranu). */
static void crt_render(void)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		if (!crt_on || !viewporter) {
			fx_free(&o->crt);
			continue;
		}
		int sc = o->bar.vp && o->bar.scale120 ? o->bar.scale120 : 120;
		int bw = (o->w * sc + 60) / 120, bh = (o->h * sc + 60) / 120;
		if (o->crt && (o->crt->bw != bw || o->crt->bh != bh))
			fx_free(&o->crt);                                   /* zmiana skali lub trybu */
		if (!o->crt) {
			struct fx *f = o->crt = fx_new(bw, bh);
			struct buf *b = buf_new(bw, bh, true, false);
			if (!b)
				continue;
			draw_crt(b);
			river_shell_surface_v1_sync_next_commit(f->ss);
			buf_attach(b, f->wl);
			wp_viewport_set_destination(f->vp, o->w, o->h);
			wl_surface_damage_buffer(f->wl, 0, 0, bw, bh);
			wl_surface_commit(f->wl);
		}
		river_node_v1_set_position(o->crt->node, o->x, o->y);
		if (!fullscreen_on(o))                                  /* film na pełnym ekranie bez pasków */
			river_node_v1_place_top(o->crt->node);
	}
}

void fx_output_fini(struct output *o)
{
	fx_free(&o->crt);
}

/* Wołane w sekwencji render, na samym końcu — warstwy lądują na wierzchu. */
void fx_render(void)
{
	crt_render();
}

static void state_path(char *buf, size_t n)
{
	const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	if (st && *st)
		snprintf(buf, n, "%s/norns/crt", st);
	else
		snprintf(buf, n, "%s/.local/state/norns/crt", home ? home : "");
}

void fx_init(void)
{
	char p[300];
	state_path(p, sizeof p);
	FILE *f = fopen(p, "r");
	if (f) {
		crt_on = fgetc(f) == '1';
		fclose(f);
	}
}

void fx_set_crt(bool on)
{
	if (on != crt_on)
		norns_notice("Linie kineskopu", on ? "włączone" : "wyłączone");
	crt_on = on;
	char p[300];
	state_path(p, sizeof p);
	char *slash = strrchr(p, '/');
	if (slash) {                                                /* ~/.local/state/norns */
		*slash = 0;
		mkdir(p, 0700);
		*slash = '/';
	}
	FILE *f = fopen(p, "w");
	if (f) {
		fputc(on ? '1' : '0', f);
		fclose(f);
	}
	river_window_manager_v1_manage_dirty(wm);
}
