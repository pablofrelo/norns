// nostromo — „tryb Nostromo”: co pół godziny glitch kineskopu, o pełnych
// godzinach dodatkowo szum radiowy i porwany głos z radiostacji.
//
// Obraz: warstwa nad całym ekranem (powierzchnia norns, przepuszcza
// kliknięcia) z przesuniętymi, podbarwionymi pasami, liniami skanowania
// i przejeżdżającym jasnym paskiem; okna szarpią w bok w tym samym rytmie.
// Treści okien nie da się pociąć — WM nie widzi ich pikseli.
// Dźwięk: norns sam generuje WAV-y i przepuszcza nagrania użytkownika
// (~/.local/share/norns/radio/*.wav, PCM 16-bit albo float) przez filtr „radiostacji”;
// odtwarza pw-play (PipeWire).
#define _GNU_SOURCE
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "norns.h"

#define RATE        48000
#define GLITCH_MS   2000            /* jak na stronie: 2 s, 40 skoków, 5 pasów */
#define JUMPS       40
#define BANDS       5
#define MAX_SHIFT   14
#define OFF_MS      900             /* zwinięcie kineskopu */
#define FX_FPS_MS   33

bool nostromo_on;

enum { FX_NONE, FX_GLITCH, FX_OFF };
static int fx_kind;
static uint64_t fx_t0, fx_last_draw;
static uint32_t fx_seed;
static char dir[256];               /* $XDG_RUNTIME_DIR/norns — wygenerowane dźwięki */

/* ---------- WAV ---------- */

static float frand(uint32_t *s)
{
	*s = *s * 1664525u + 1013904223u;
	return (float)(*s >> 8) / 16777216.0f * 2.0f - 1.0f;     /* -1..1 */
}

static bool wav_write(const char *path, const float *x, int n, int rate)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return false;
	uint32_t data = n * 2, riff = 36 + data, fmt = 16, byterate = rate * 2;
	uint16_t pcm = 1, ch = 1, align = 2, bits = 16;
	fwrite("RIFF", 1, 4, f); fwrite(&riff, 4, 1, f); fwrite("WAVEfmt ", 1, 8, f);
	fwrite(&fmt, 4, 1, f); fwrite(&pcm, 2, 1, f); fwrite(&ch, 2, 1, f);
	fwrite(&rate, 4, 1, f); fwrite(&byterate, 4, 1, f); fwrite(&align, 2, 1, f); fwrite(&bits, 2, 1, f);
	fwrite("data", 1, 4, f); fwrite(&data, 4, 1, f);
	for (int i = 0; i < n; i++) {
		float v = x[i] > 1 ? 1 : x[i] < -1 ? -1 : x[i];
		int16_t s = (int16_t)(v * 32767);
		fwrite(&s, 2, 1, f);
	}
	return fclose(f) == 0;
}

/* PCM 16-bit, dowolna liczba kanałów i częstotliwość — miks do mono. */
static float *wav_read(const char *path, int *n, int *rate)
{
	FILE *f = fopen(path, "rb");
	if (!f)
		return NULL;
	char id[4];
	uint32_t sz;
	uint16_t fmt = 0, ch = 0, bits = 0;
	uint32_t r = 0;
	float *out = NULL;
	if (fread(id, 1, 4, f) != 4 || memcmp(id, "RIFF", 4) || fread(&sz, 4, 1, f) != 1 ||
	    fread(id, 1, 4, f) != 4 || memcmp(id, "WAVE", 4))
		goto done;
	while (fread(id, 1, 4, f) == 4 && fread(&sz, 4, 1, f) == 1) {
		if (!memcmp(id, "fmt ", 4)) {
			uint8_t b[40] = { 0 };
			if (sz > sizeof b || fread(b, 1, sz, f) != sz)
				goto done;
			memcpy(&fmt, b, 2);
			memcpy(&ch, b + 2, 2);
			memcpy(&r, b + 4, 4);
			memcpy(&bits, b + 14, 2);
		} else if (!memcmp(id, "data", 4)) {
			bool f32 = fmt == 3 && bits == 32;                  /* float — tak też zapisuje pw-record */
			if (!(fmt == 1 && bits == 16) && !f32)
				goto done;
			if (ch == 0)
				goto done;
			int bps = bits / 8, frames = sz / (bps * ch);
			uint8_t *raw = malloc(sz);
			out = malloc(sizeof *out * (frames ? frames : 1));
			if (!raw || !out || fread(raw, 1, sz, f) != sz) {
				free(raw);
				free(out);
				out = NULL;
				goto done;
			}
			for (int i = 0; i < frames; i++) {
				float s = 0;
				for (int c = 0; c < ch; c++) {
					if (f32) {
						float v;
						memcpy(&v, raw + (i * ch + c) * 4, 4);
						s += v;
					} else {
						int16_t v;
						memcpy(&v, raw + (i * ch + c) * 2, 2);
						s += v / 32768.0f;
					}
				}
				out[i] = s / ch;
			}
			free(raw);
			*n = frames;
			*rate = r;
			goto done;
		} else {
			fseek(f, sz + (sz & 1), SEEK_CUR);
		}
	}
done:
	fclose(f);
	return out;
}

/* Biquad (RBJ): górno- i dolnoprzepustowy — pasmo krótkofalówki. */
struct biquad { float b0, b1, b2, a1, a2, x1, x2, y1, y2; };

static struct biquad bq(int rate, float freq, bool high)
{
	float w = 2 * (float)M_PI * freq / rate, c = cosf(w), al = sinf(w) / (2 * 0.707f), a0 = 1 + al;
	struct biquad q = { 0 };
	q.b0 = (high ? (1 + c) / 2 : (1 - c) / 2) / a0;
	q.b1 = (high ? -(1 + c) : (1 - c)) / a0;
	q.b2 = q.b0;
	q.a1 = -2 * c / a0;
	q.a2 = (1 - al) / a0;
	return q;
}

static float bq_run(struct biquad *q, float x)
{
	float y = q->b0 * x + q->b1 * q->x1 + q->b2 * q->x2 - q->a1 * q->y1 - q->a2 * q->y2;
	q->x2 = q->x1;
	q->x1 = x;
	q->y2 = q->y1;
	q->y1 = y;
	return y;
}

/* ---------- dźwięki ---------- */

/* Bzyknięcie: przetwornica 50 Hz z szumem, poszarpana. */
static void gen_buzz(const char *path)
{
	int n = RATE * 45 / 100;
	float *x = calloc(n, sizeof *x);
	uint32_t s = 7;
	float gate = 1;
	for (int i = 0; i < n; i++) {
		if (i % (RATE / 50) == 0)
			gate = frand(&s) > -0.3f ? 1 : 0.15f;          /* rwanie co 20 ms */
		float t = (float)i / RATE, env = fminf(1, t * 40) * fminf(1, (0.45f - t) * 20);
		float sq = sinf(2 * (float)M_PI * 50 * t) > 0 ? 1 : -1;
		x[i] = env * gate * (0.22f * sq + 0.12f * sinf(2 * (float)M_PI * 100 * t) + 0.10f * frand(&s));
	}
	wav_write(path, x, n, RATE);
	free(x);
}

/* Szum radiowy: przefiltrowany szum, trzaski, lekkie falowanie. */
static void gen_static(const char *path, float secs)
{
	int n = (int)(RATE * secs);
	float *x = calloc(n, sizeof *x);
	uint32_t s = 11;
	struct biquad hp = bq(RATE, 400, true), lp = bq(RATE, 3200, false);
	for (int i = 0; i < n; i++) {
		float t = (float)i / RATE, env = fminf(1, t * 8) * fminf(1, (secs - t) * 6);
		float v = bq_run(&lp, bq_run(&hp, frand(&s)));
		v *= 0.75f + 0.25f * sinf(2 * (float)M_PI * 6.5f * t);
		if (frand(&s) > 0.9993f)
			v += frand(&s) * 2.5f;                           /* trzask */
		x[i] = env * 0.55f * v;
	}
	wav_write(path, x, n, RATE);
	free(x);
}

/* Gaśnięcie radia: trzask przełącznika, opadający ton i zanikający szum. */
static void gen_off(const char *path)
{
	int n = RATE * 12 / 10;
	float *x = calloc(n, sizeof *x), ph = 0;
	uint32_t s = 23;
	struct biquad lp = bq(RATE, 2500, false);
	for (int i = 0; i < n; i++) {
		float t = (float)i / RATE;
		float click = t < 0.006f ? frand(&s) * (1 - t / 0.006f) : 0;
		float f = 900 * powf(60.0f / 900, fminf(1, t / 1.0f));  /* 900 → 60 Hz */
		ph += 2 * (float)M_PI * f / RATE;
		float decay = expf(-t * 3.2f);
		x[i] = 0.8f * click + decay * (0.18f * sinf(ph) + 0.25f * bq_run(&lp, frand(&s)));
	}
	wav_write(path, x, n, RATE);
	free(x);
}

/* Nagranie → radiostacja: pasmo 300–3000 Hz, przester, zaniki, szum pod spodem. */
static bool radioize(const char *in, const char *out)
{
	int n, rate;
	float *x = wav_read(in, &n, &rate);
	if (!x || n <= 0 || rate < 8000) {
		free(x);
		return false;
	}
	float peak = 0.01f;
	for (int i = 0; i < n; i++)
		peak = fmaxf(peak, fabsf(x[i]));
	int pre = rate * 3 / 10, total = pre + n + rate / 5;
	float *y = calloc(total, sizeof *y);
	struct biquad hp = bq(rate, 300, true), hp2 = bq(rate, 300, true);
	struct biquad lp = bq(rate, 3000, false), lp2 = bq(rate, 3000, false);
	struct biquad nlp = bq(rate, 3500, false);
	uint32_t s = 99;
	int drop = 0, next = rate / 2;
	for (int i = 0; i < total; i++) {
		float v = 0;
		int j = i - pre;
		if (j >= 0 && j < n) {
			v = x[j] / peak;
			v = bq_run(&lp2, bq_run(&lp, bq_run(&hp2, bq_run(&hp, v))));
			v = tanhf(v * 3.5f) / tanhf(3.5f);              /* przester */
			if (--next <= 0) {                               /* co 0,4–1,2 s zanik 40–140 ms */
				drop = rate * (40 + (int)((frand(&s) + 1) * 50)) / 1000;
				next = rate * (400 + (int)((frand(&s) + 1) * 400)) / 1000;
			}
			if (drop > 0) {
				drop--;
				v *= 0.05f;
			}
		}
		float noise = bq_run(&nlp, frand(&s)) * (j < 0 || j >= n ? 0.35f : 0.12f);
		y[i] = 0.7f * v + noise;
	}
	bool ok = wav_write(out, y, total, rate);
	free(x);
	free(y);
	return ok;
}

static void sound_path(char *buf, size_t n, const char *name)
{
	snprintf(buf, n, "%s/%s", dir, name);
}

/* Odtwarzanie w tle: kolejne pliki po kolei (sh -c), cicho. */
static void play(const char *script)
{
	if (fork() == 0) {
		setsid();
		execl("/bin/sh", "sh", "-c", script, (char *)NULL);
		_exit(1);
	}
}

/* Losowe nagranie użytkownika przerobione na radio; NULL = brak nagrań. */
static bool pick_voice(char *out, size_t n)
{
	char vdir[300];
	const char *data = getenv("XDG_DATA_HOME"), *home = getenv("HOME");
	if (data && *data)
		snprintf(vdir, sizeof vdir, "%s/norns/radio", data);
	else
		snprintf(vdir, sizeof vdir, "%s/.local/share/norns/radio", home ? home : "");
	DIR *d = opendir(vdir);
	if (!d)
		return false;
	char names[32][256];
	int cnt = 0;
	struct dirent *e;
	while ((e = readdir(d)) && cnt < 32) {
		size_t l = strlen(e->d_name);
		if (l > 4 && !strcasecmp(e->d_name + l - 4, ".wav"))
			snprintf(names[cnt++], sizeof names[0], "%s", e->d_name);
	}
	closedir(d);
	if (!cnt)
		return false;
	char in[600];
	snprintf(in, sizeof in, "%s/%s", vdir, names[rand() % cnt]);
	sound_path(out, n, "voice.wav");
	if (!radioize(in, out)) {
		fprintf(stderr, "norns: nagranie %s nie jest WAV PCM 16-bit ani float — pomijam\n", in);
		return false;
	}
	return true;
}

static void play_radio(bool voice)
{
	char st[300], vo[300], cmd[1200];
	sound_path(st, sizeof st, "static.wav");
	if (voice && pick_voice(vo, sizeof vo))
		snprintf(cmd, sizeof cmd, "pw-play --volume 0.35 '%s' & sleep 0.6; pw-play --volume 0.5 '%s'", st, vo);
	else
		snprintf(cmd, sizeof cmd, "pw-play --volume 0.35 '%s'", st);
	play(cmd);
}

static void play_file(const char *name, float vol)
{
	char p[300], cmd[400];
	sound_path(p, sizeof p, name);
	snprintf(cmd, sizeof cmd, "pw-play --volume %.2f '%s'", vol, p);
	play(cmd);
}

/* ---------- warstwa efektów nad ekranem ---------- */

struct fx {
	struct wl_surface *wl;
	struct river_shell_surface_v1 *ss;
	struct river_node_v1 *node;
	struct wp_viewport *vp;
	struct buf *b[2];
	int bw, bh;
};

void nostromo_output_fini(struct output *o)
{
	struct fx *f = o->fx;
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
	o->fx = NULL;
}

static struct fx *fx_get(struct output *o)
{
	if (o->fx)
		return o->fx;
	struct fx *f = calloc(1, sizeof *f);
	f->wl = wl_compositor_create_surface(compositor);
	struct wl_region *none = wl_compositor_create_region(compositor);
	wl_surface_set_input_region(f->wl, none);                   /* kliknięcia przechodzą */
	wl_region_destroy(none);
	f->ss = river_window_manager_v1_get_shell_surface(wm, f->wl);
	f->node = river_shell_surface_v1_get_node(f->ss);
	if (viewporter) {                                           /* pół rozdzielczości, rozciągnięte */
		f->vp = wp_viewporter_get_viewport(viewporter, f->wl);
		f->bw = o->w / 2;
		f->bh = o->h / 2;
	} else {
		f->bw = o->w;
		f->bh = o->h;
	}
	o->fx = f;
	return f;
}

static void px_fill(struct buf *b, int x, int y, int w, int h, uint32_t argb_premul)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > b->w) w = b->w - x;
	if (y + h > b->h) h = b->h - y;
	uint32_t *p = b->data;
	for (int j = y; j < y + h; j++)
		for (int i = x; i < x + w; i++)
			p[j * b->w + i] = argb_premul;
}

static uint32_t premul(uint32_t rgb, float a)
{
	int A = (int)(a * 255);
	if (A < 0) A = 0;
	if (A > 255) A = 255;
	return (uint32_t)A << 24 | (uint32_t)(((rgb >> 16) & 0xff) * A / 255) << 16 |
	       (uint32_t)(((rgb >> 8) & 0xff) * A / 255) << 8 | (uint32_t)((rgb & 0xff) * A / 255);
}

/* Klatka glitcha: pasy, linie skanowania, jasny pasek przejeżdżający z góry na dół. */
static void draw_glitch(struct buf *b, double p, int step)
{
	memset(b->data, 0, b->size);
	float s = sinf((float)(p * M_PI));
	uint32_t fg = THEME->fg;
	for (int y = 0; y < b->h; y += 3)                           /* linie skanowania */
		px_fill(b, 0, y, b->w, 1, premul(0x000000, 0.22f * s));
	uint32_t seed = fx_seed ^ (uint32_t)step * 2654435761u;
	for (int i = 0; i < BANDS; i++) {
		float r1 = (frand(&seed) + 1) / 2, r2 = (frand(&seed) + 1) / 2, r3 = frand(&seed);
		int h = (int)(b->h * r1 * 0.06f) + 2, y = (int)(r2 * (b->h - h));
		int dx = (int)(r3 * MAX_SHIFT * s * b->w / 1280.0f);
		px_fill(b, dx, y, b->w, h, premul(fg, 0.10f + 0.15f * s));
		px_fill(b, 0, y, b->w, 1, premul(fg, 0.45f * s));       /* jasna krawędź pasa */
	}
	int bar_h = b->h / 12, bar_y = (int)(p * (b->h + bar_h)) - bar_h;
	px_fill(b, 0, bar_y, b->w, bar_h, premul(fg, 0.08f));
}

/* Zwinięcie kineskopu: czerń dookoła, obraz zwęża się do linii, potem do kropki. */
static void draw_off(struct buf *b, double p)
{
	uint32_t black = 0xff000000, glow = premul(THEME->fg, 1.0f);
	px_fill(b, 0, 0, b->w, b->h, black);
	int cx = b->w / 2, cy = b->h / 2;
	if (p < 0.45) {                                             /* pionowo: ekran → linia */
		double e = p / 0.45, k = 1 - e * e;
		int h = (int)(b->h * k);
		if (h < 2)
			h = 2;
		px_fill(b, 0, cy - h / 2, b->w, h, premul(THEME->fg, 0.12f + 0.6f * (float)e));
	} else if (p < 0.75) {                                      /* poziomo: linia → kropka */
		double e = (p - 0.45) / 0.30;
		int w = (int)(b->w * (1 - e));
		if (w < 4)
			w = 4;
		px_fill(b, cx - w / 2, cy - 1, w, 3, glow);
	} else {                                                    /* kropka gaśnie */
		float a = (float)(1 - (p - 0.75) / 0.25);
		px_fill(b, cx - 2, cy - 2, 5, 5, premul(THEME->fg, a));
	}
}

static bool fx_active(uint64_t now)
{
	int len = fx_kind == FX_GLITCH ? GLITCH_MS : fx_kind == FX_OFF ? OFF_MS : 0;
	return fx_kind != FX_NONE && now < fx_t0 + len;
}

/* Przesunięcie okna w tej klatce glitcha (ten sam rytm skoków co pasy). */
int nostromo_jitter(const struct win *w, uint64_t now)
{
	if (fx_kind != FX_GLITCH || !fx_active(now))
		return 0;
	double p = (double)(now - fx_t0) / GLITCH_MS;
	int step = (int)(p * JUMPS);
	uint32_t seed = fx_seed ^ (uint32_t)(uintptr_t)w ^ (uint32_t)step * 40503u;
	frand(&seed);
	return (int)(frand(&seed) * MAX_SHIFT * sinf((float)(p * M_PI)));
}

bool nostromo_busy(uint64_t now)
{
	return fx_active(now);
}

/* Wołane w sekwencji render, po pasku — warstwa ląduje na samej górze. */
void nostromo_render(uint64_t now)
{
	struct output *o;
	if (fx_kind == FX_NONE)
		return;
	if (!fx_active(now)) {                                      /* koniec: sprzątamy warstwy */
		wl_list_for_each(o, &outputs, link)
			nostromo_output_fini(o);
		fx_kind = FX_NONE;
		return;
	}
	if (now < fx_last_draw + FX_FPS_MS)
		return;
	fx_last_draw = now;
	int len = fx_kind == FX_GLITCH ? GLITCH_MS : OFF_MS;
	double p = (double)(now - fx_t0) / len;
	wl_list_for_each(o, &outputs, link) {
		struct fx *f = fx_get(o);
		int i = f->b[0] && f->b[0]->busy ? 1 : 0;
		if (!f->b[i])
			f->b[i] = buf_new(f->bw, f->bh, true, true);
		if (!f->b[i] || f->b[i]->busy)
			continue;
		if (fx_kind == FX_GLITCH)
			draw_glitch(f->b[i], p, (int)(p * JUMPS));
		else
			draw_off(f->b[i], p);
		river_shell_surface_v1_sync_next_commit(f->ss);
		buf_attach(f->b[i], f->wl);
		if (f->vp)
			wp_viewport_set_destination(f->vp, o->w, o->h);
		wl_surface_damage_buffer(f->wl, 0, 0, f->bw, f->bh);
		wl_surface_commit(f->wl);
		river_node_v1_set_position(f->node, o->x, o->y);
		river_node_v1_place_top(f->node);
	}
}

static void fx_start(int kind)
{
	fx_kind = kind;
	fx_t0 = now_ms();
	fx_last_draw = 0;
	fx_seed = (uint32_t)fx_t0 * 2246822519u;
	river_window_manager_v1_manage_dirty(wm);                   /* ruszamy klatki */
}

/* ---------- sterowanie ---------- */

static void state_path(char *buf, size_t n)
{
	const char *st = getenv("XDG_STATE_HOME"), *home = getenv("HOME");
	if (st && *st)
		snprintf(buf, n, "%s/norns/nostromo", st);
	else
		snprintf(buf, n, "%s/.local/state/norns/nostromo", home ? home : "");
}

void nostromo_init(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(dir, sizeof dir, "%s/norns", rt && *rt ? rt : "/tmp");
	mkdir(dir, 0700);
	char p[300];
	sound_path(p, sizeof p, "buzz.wav");
	gen_buzz(p);
	sound_path(p, sizeof p, "static.wav");
	gen_static(p, 2.5f);
	sound_path(p, sizeof p, "off.wav");
	gen_off(p);
	srand((unsigned)time(NULL));

	char sp[300];
	state_path(sp, sizeof sp);
	FILE *f = fopen(sp, "r");
	if (f) {
		nostromo_on = fgetc(f) == '1';
		fclose(f);
	}
}

void nostromo_toggle(void)
{
	nostromo_on = !nostromo_on;
	char sp[300];
	state_path(sp, sizeof sp);
	FILE *f = fopen(sp, "w");
	if (f) {
		fputc(nostromo_on ? '1' : '0', f);
		fclose(f);
	}
	if (nostromo_on) {                                          /* od razu pokaz */
		fx_start(FX_GLITCH);
		play_file("buzz.wav", 0.4f);
		play_radio(true);
	} else {                                                    /* radio gaśnie, kineskop się zwija */
		fx_start(FX_OFF);
		play_file("off.wav", 0.45f);
	}
}

/* Wołane co 2 s: HH:00 — glitch, szum i głos; HH:30 — glitch z bzyknięciem. */
void nostromo_tick(bool fullscreen_visible)
{
	static int last = -1;
	time_t t = time(NULL);
	struct tm *tm = localtime(&t);
	int slot = tm->tm_min == 0 ? tm->tm_hour * 2 : tm->tm_min == 30 ? tm->tm_hour * 2 + 1 : -1;
	if (slot < 0 || slot == last)
		return;
	last = slot;
	if (!nostromo_on || !anim_on || fullscreen_visible || fx_kind != FX_NONE)
		return;
	fx_start(FX_GLITCH);
	play_file("buzz.wav", 0.35f);
	if (tm->tm_min == 0)
		play_radio(true);
}
