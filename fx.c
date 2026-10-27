// fx — warstwa efektów nad całym ekranem: powierzchnia norns na samej górze,
// przepuszcza kliknięcia, rysowana w fizycznej rozdzielczości monitora.
// Efekty: linie kineskopu (Super+Ctrl+A, norns-ctl crt) i wygaszacz
// z kafli BSP (norns-ctl saver).
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

/* Powierzchnia nad wszystkim; bufor bw×bh rozciągany na cały monitor.
 * input = false: kliknięcia przechodzą do okien pod spodem. */
static struct fx *fx_new(int bw, int bh, bool input)
{
	struct fx *f = calloc(1, sizeof *f);
	f->wl = wl_compositor_create_surface(compositor);
	if (!input) {
		struct wl_region *none = wl_compositor_create_region(compositor);
		wl_surface_set_input_region(f->wl, none);
		wl_region_destroy(none);
	}
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
			struct fx *f = o->crt = fx_new(bw, bh, false);
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

/* ---------- wygaszacz: kafle BSP ---------- */

/* Ekran to jeden kafel. Co SAVER_STEP_MS losowy kafel dzieli się na pół wzdłuż
 * dłuższego boku (jak nowe okno w norns) albo para sąsiednich liści zwija się
 * z powrotem w jeden. Kolory z motywu, najnowszy kafel ma ramkę fokusu.
 * Włącza norns-ctl saver (np. ze swayidle); budzi ruch myszy, klik, skrót norns. */

bool saver_on;
uint64_t saver_since;

#define SV_NODES (2 * SAVER_MAX + 2)

struct snode {
	int a, b;                       /* dzieci; a < 0 — liść */
	bool sbs, closing;              /* podział w poziomie; zwija się do jednego dziecka */
	float from, to;                 /* udział dziecka a: skąd i dokąd jedzie */
	uint64_t t0;
	int w, h;                       /* ostatni rozmiar (piksele bufora) */
};

struct saver {
	struct fx *f;
	struct snode n[SV_NODES];
	bool used[SV_NODES];
	int newest;                     /* kafel z ramką fokusu */
	uint64_t next;                  /* następny krok */
	bool dirty;
};

/* Pixman przy jednolitym wypełnieniu nie przycina — robimy to sami. */
static void box(pixman_image_t *img, int x, int y, int w, int h, uint32_t rgb)
{
	int iw = pixman_image_get_width(img), ih = pixman_image_get_height(img);
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > iw) w = iw - x;
	if (y + h > ih) h = ih - y;
	if (w <= 0 || h <= 0)
		return;
	pixman_color_t c = {
		((rgb >> 16) & 0xff) * 0x101, ((rgb >> 8) & 0xff) * 0x101, (rgb & 0xff) * 0x101, 0xffff,
	};
	pixman_rectangle16_t r = { x, y, w, h };
	pixman_image_fill_rectangles(PIXMAN_OP_SRC, img, &c, 1, &r);
}

static bool sv_moving(const struct snode *n, uint64_t now)
{
	return anim_on && now < n->t0 + SAVER_MS;
}

static float sv_ratio(const struct snode *n, uint64_t now)
{
	if (!sv_moving(n, now))
		return n->to;
	float p = (float)(now - n->t0) / SAVER_MS, e = 1 - (1 - p) * (1 - p) * (1 - p);
	return n->from + (n->to - n->from) * e;
}

static int sv_alloc(struct saver *s)
{
	for (int i = 1; i < SV_NODES; i++)
		if (!s->used[i]) {
			s->used[i] = true;
			memset(&s->n[i], 0, sizeof s->n[i]);
			s->n[i].a = -1;
			return i;
		}
	return -1;
}

/* Rysuje drzewo od węzła i; g — przerwa, bd — ramka (piksele bufora). */
static bool sv_draw(struct saver *s, int i, pixman_image_t *img, int x, int y, int w, int h,
                    int g, int bd, uint64_t now)
{
	struct snode *n = &s->n[i];
	n->w = w;
	n->h = h;
	if (n->a < 0) {
		if (w > 2 * (g + bd) && h > 2 * (g + bd)) {
			box(img, x + g, y + g, w - 2 * g, h - 2 * g, i == s->newest ? THEME->focus : THEME->unfocus);
			box(img, x + g + bd, y + g + bd, w - 2 * (g + bd), h - 2 * (g + bd), THEME->bg);
		}
		return false;
	}
	float r = sv_ratio(n, now);
	bool m = sv_moving(n, now);
	if (n->sbs) {
		int aw = (int)(w * r);
		m |= sv_draw(s, n->a, img, x, y, aw, h, g, bd, now);
		m |= sv_draw(s, n->b, img, x + aw, y, w - aw, h, g, bd, now);
	} else {
		int ah = (int)(h * r);
		m |= sv_draw(s, n->a, img, x, y, w, ah, g, bd, now);
		m |= sv_draw(s, n->b, img, x, y + ah, w, h - ah, g, bd, now);
	}
	return m;
}

/* Kolejny krok: kończy zwijanie, potem (co SAVER_STEP_MS) dzieli albo zwija. */
static void sv_step(struct saver *s, uint64_t now, int minpx)
{
	for (int i = 0; i < SV_NODES; i++) {
		struct snode *n = &s->n[i];
		if (s->used[i] && n->a >= 0 && n->closing && !sv_moving(n, now)) {
			s->used[n->a] = s->used[n->b] = false;
			n->a = -1;
			n->closing = false;
			s->newest = i;
			s->dirty = true;
		}
	}
	if (now < s->next)
		return;
	s->next = now + SAVER_STEP_MS;

	int leaves = 0, split[SV_NODES], ns = 0, fold[SV_NODES], nf = 0;
	for (int i = 0; i < SV_NODES; i++) {
		struct snode *n = &s->n[i];
		if (!s->used[i])
			continue;
		if (n->a < 0) {
			leaves++;
			if (n->w >= 2 * minpx || n->h >= 2 * minpx)
				split[ns++] = i;
		} else if (!n->closing && !sv_moving(n, now) && s->n[n->a].a < 0 && s->n[n->b].a < 0) {
			fold[nf++] = i;
		}
	}
	bool grow = ns && (!nf || leaves < 3 || (leaves < SAVER_MAX && rand() % 3));
	if (grow) {
		int i = split[rand() % ns], a = sv_alloc(s), b = a < 0 ? -1 : sv_alloc(s);
		if (b < 0) {
			if (a >= 0)
				s->used[a] = false;
			return;
		}
		struct snode *n = &s->n[i];
		n->a = a;
		n->b = b;
		n->sbs = n->w >= n->h;
		n->from = 1;                /* nowy kafel wjeżdża od prawej / od dołu */
		n->to = 0.5f;
		n->t0 = now;
		s->newest = b;
	} else if (nf) {
		struct snode *n = &s->n[fold[rand() % nf]];
		n->closing = true;
		n->from = n->to;
		n->to = rand() % 2 ? 1 : 0;   /* jedno dziecko zjada drugie */
		n->t0 = now;
		s->newest = n->to > 0.5f ? n->a : n->b;
	}
	s->dirty = true;
}

static void sv_free(struct saver **slot)
{
	if (!*slot)
		return;
	fx_free(&(*slot)->f);
	free(*slot);
	*slot = NULL;
}

/* Rysuje klatkę do wolnego bufora; true — coś jeszcze jedzie. */
static bool sv_paint(struct output *o, uint64_t now, bool sync)
{
	struct saver *s = o->saver;
	struct fx *f = s->f;
	struct buf **slot = f->b[0] && f->b[0]->busy ? &f->b[1] : &f->b[0];
	if (*slot && (*slot)->busy)
		return true;                /* oba u compositora — spróbujemy za klatkę */
	if (!*slot && !(*slot = buf_new(f->bw, f->bh, false, true)))
		return false;
	struct buf *b = *slot;
	int sc = o->bar.vp && o->bar.scale120 ? o->bar.scale120 : 120;
	int g = GAP * sc / 240, bd = BORDER * sc / 120 > 0 ? BORDER * sc / 120 : 1;
	box(b->img, 0, 0, b->w, b->h, THEME->wall);
	bool m = sv_draw(s, 0, b->img, g, g, b->w - 2 * g, b->h - 2 * g, g, bd, now);
	if (sync)
		river_shell_surface_v1_sync_next_commit(f->ss);
	buf_attach(b, f->wl);
	wp_viewport_set_destination(f->vp, o->w, o->h);
	wl_surface_damage_buffer(f->wl, 0, 0, b->w, b->h);
	wl_surface_commit(f->wl);
	return m;
}

/* W sekwencji render: powierzchnie wygaszacza na samej górze (także nad pełnym ekranem). */
static void saver_render(void)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		if (!saver_on || !viewporter) {
			sv_free(&o->saver);
			continue;
		}
		int sc = o->bar.vp && o->bar.scale120 ? o->bar.scale120 : 120;
		int bw = (o->w * sc + 60) / 120, bh = (o->h * sc + 60) / 120;
		if (o->saver && (o->saver->f->bw != bw || o->saver->f->bh != bh))
			sv_free(&o->saver);                             /* zmiana skali lub trybu */
		if (!o->saver) {
			struct saver *s = o->saver = calloc(1, sizeof *s);
			s->f = fx_new(bw, bh, true);                    /* kliki trafiają w wygaszacz */
			s->used[0] = true;
			s->n[0].a = -1;
			s->next = now_ms() + SAVER_STEP_MS / 2;
			s->dirty = sv_paint(o, now_ms(), true);
		}
		river_node_v1_set_position(o->saver->f->node, o->x, o->y);
		river_node_v1_place_top(o->saver->f->node);
	}
}

/* Poza sekwencjami, z własnego budzika w main(): kroki i klatki ruchu. */
int fx_saver_frame(uint64_t now)
{
	if (!saver_on)
		return 0;
	int want = SAVER_STEP_MS;
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		struct saver *s = o->saver;
		if (!s) {
			want = 16;              /* powierzchnia powstanie w najbliższym render */
			continue;
		}
		int sc = o->bar.vp && o->bar.scale120 ? o->bar.scale120 : 120;
		sv_step(s, now, SAVER_MIN * sc / 120);
		if (s->dirty)
			s->dirty = sv_paint(o, now, false);
		int left = s->dirty ? 16 : (int)(s->next > now ? s->next - now : 16);
		if (left < want)
			want = left;
	}
	return want < 16 ? 16 : want;
}

void fx_set_saver(bool on)
{
	if (on && !saver_on) {
		saver_since = now_ms();
		srand((unsigned)saver_since);   /* za każdym razem inny układ */
	}
	saver_on = on;
	river_window_manager_v1_manage_dirty(wm);
}

void fx_output_fini(struct output *o)
{
	fx_free(&o->crt);
	sv_free(&o->saver);
}

/* Wołane w sekwencji render, na samym końcu — warstwy lądują na wierzchu
 * (linie kineskopu także nad wygaszaczem). */
void fx_render(void)
{
	saver_render();
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
