// sfx — dźwięki norns generowane w C i odtwarzane przez pw-play (PipeWire):
// stukot klawiszy w powiadomieniach, serwo terminala wysuwanego, migawka zrzutów.
// Pliki WAV powstają przy starcie w $XDG_RUNTIME_DIR/norns/.
#define _GNU_SOURCE
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "norns.h"

#define RATE        48000

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

static void play_file(const char *name, float vol)
{
	char p[300], cmd[400];
	sound_path(p, sizeof p, name);
	snprintf(cmd, sizeof cmd, "pw-play --volume %.2f '%s'", vol, p);
	play(cmd);
}

/* Serwo grodzi z Nostromo: syk pneumatyki, silniczek zmieniający ton w trakcie
 * ruchu i głuche „klunk” na końcu. up — wysuwanie (ton rośnie), inaczej chowanie. */
static void gen_servo(const char *path, bool up)
{
	float secs = DROP_MS / 1000.0f + 0.15f, move = DROP_MS / 1000.0f;
	int n = (int)(RATE * secs);
	float *x = calloc(n, sizeof *x), ph = 0, ph2 = 0;
	uint32_t s = up ? 31 : 37;
	struct biquad hiss_hp = bq(RATE, 2500, true), hiss_lp = bq(RATE, 7000, false);
	struct biquad mot_lp = bq(RATE, 1800, false), thud_lp = bq(RATE, 300, false);
	for (int i = 0; i < n; i++) {
		float t = (float)i / RATE, p = fminf(1, t / move);
		float e = 1 - (1 - p) * (1 - p) * (1 - p);                  /* to samo hamowanie co ślizg */
		float f = up ? 140 + 160 * e : 300 - 160 * e;               /* silnik: ton za ruchem */
		ph += 2 * (float)M_PI * f / RATE;
		ph2 += 2 * (float)M_PI * f * 2.01f / RATE;
		float saw = fmodf(ph / (2 * (float)M_PI), 1) * 2 - 1;
		float menv = t < move ? fminf(1, t * 30) * (1 - 0.7f * e) : 0;
		float motor = bq_run(&mot_lp, 0.5f * saw + 0.3f * sinf(ph2)) * menv;
		float henv = expf(-t * 7) * fminf(1, t * 200);              /* syk na starcie */
		float hiss = bq_run(&hiss_lp, bq_run(&hiss_hp, frand(&s))) * henv;
		float te = t - move, thud = 0;
		if (te >= 0) {                                              /* klunk: dociśnięcie */
			float k = expf(-te * 45);
			thud = k * (0.9f * sinf(2 * (float)M_PI * (up ? 85 : 65) * te) + 0.6f * bq_run(&thud_lp, frand(&s)));
			if (te < 0.004f)
				thud += frand(&s) * (1 - te / 0.004f) * 0.5f;
		}
		x[i] = 0.35f * motor + 0.25f * hiss + 0.7f * thud;
	}
	wav_write(path, x, n, RATE);
	free(x);
}

/* Migawka: dwa suche kliknięcia (otwarcie, zamknięcie) i krótki szum przesuwu. */
static void gen_shutter(const char *path)
{
	int n = RATE * 18 / 100;
	float *x = calloc(n, sizeof *x);
	uint32_t s = 41;
	struct biquad hp = bq(RATE, 1200, true);
	for (int i = 0; i < n; i++) {
		float t = (float)i / RATE, v = 0;
		for (int k = 0; k < 2; k++) {                       /* klik 0 ms i 70 ms */
			float tk = t - k * 0.07f;
			if (tk >= 0)
				v += expf(-tk * 600) * frand(&s) * (k ? 0.8f : 1.0f)
				     + expf(-tk * 120) * sinf(2 * (float)M_PI * (k ? 520 : 380) * tk) * 0.3f;
		}
		if (t < 0.07f)
			v += 0.12f * frand(&s) * sinf((float)M_PI * t / 0.07f);   /* przesuw migawki */
		x[i] = 0.7f * bq_run(&hp, v);
	}
	wav_write(path, x, n, RATE);
	free(x);
}

const char *sfx_path(const char *name)
{
	static char p[300];
	sound_path(p, sizeof p, name);
	return p;
}

void sfx_drop(bool up)
{
	play_file(up ? "drop_up.wav" : "drop_down.wav", 0.45f);
}

/* Stukot klawiszy do powiadomienia: jeden klik na każdy pisany znak (spacje
 * cicho), w tym samym tempie co tekst na ekranie. */
void sfx_typing(const char *text, int ms)
{
	int chars = 0;
	for (const char *p = text; *p; p++)
		if ((*p & 0xc0) != 0x80)
			chars++;
	if (chars == 0 || ms <= 0 || !*dir)
		return;
	int n = RATE * (ms + 80) / 1000;
	float *x = calloc(n, sizeof *x);
	uint32_t s = (uint32_t)now_ms() | 1;
	struct biquad bp_hi = bq(RATE, 1800, true), bp_lo = bq(RATE, 5200, false);
	int k = 0;
	for (const char *p = text; *p; p++) {
		if ((*p & 0xc0) == 0x80)
			continue;
		int at = (int)((int64_t)k++ * ms / chars * RATE / 1000);
		if (*p == ' ' || *p == '\n')
			continue;
		at += (int)(frand(&s) * RATE * 0.004f);                  /* nierówna ręka */
		float amp = 0.55f + 0.25f * frand(&s), tone = 0.8f + 0.2f * frand(&s);
		int len = RATE * 12 / 1000;
		for (int i = 0; i < len && at + i < n; i++) {
			if (at + i < 0)
				continue;
			float t = (float)i / RATE, env = expf(-t * 900);       /* trzask ~3 ms */
			float thump = expf(-t * 300) * sinf(2 * (float)M_PI * 180 * tone * t);
			x[at + i] += amp * (env * frand(&s) + 0.35f * thump);
		}
	}
	float peak = 0.01f;
	for (int i = 0; i < n; i++) {                               /* bez brzęczenia i dudnienia */
		x[i] = bq_run(&bp_lo, bq_run(&bp_hi, x[i]));
		peak = fmaxf(peak, fabsf(x[i]));
	}
	for (int i = 0; i < n; i++)
		x[i] *= 0.7f / peak;
	char path[300], cmd[400];
	sound_path(path, sizeof path, "keys.wav");
	if (wav_write(path, x, n, RATE)) {
		snprintf(cmd, sizeof cmd, "pw-play --volume 0.5 '%s'", path);
		play(cmd);
	}
	free(x);
}

void sfx_init(void)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(dir, sizeof dir, "%s/norns", rt && *rt ? rt : "/tmp");
	mkdir(dir, 0700);
	char p[300];
	sound_path(p, sizeof p, "drop_up.wav");
	gen_servo(p, true);
	sound_path(p, sizeof p, "drop_down.wav");
	gen_servo(p, false);
	sound_path(p, sizeof p, "shutter.wav");
	gen_shutter(p);
}
