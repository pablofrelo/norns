// norns — BSP window manager for river (river-window-management-v1)
#define _POSIX_C_SOURCE 200809L
#define _DEFAULT_SOURCE             /* strsep */
#include <fcntl.h>
#include <limits.h>
#include <linux/input-event-codes.h>
#include <locale.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#define NORNS_MAIN                  /* config.h: tablice tylko w tym pliku */
#include <errno.h>
#include <systemd/sd-bus.h>
#include "norns.h"

int theme_idx;

/* Konfiguracja (skróty, reguły, motywy, przerwy) siedzi w config.h. */

#define C8(x)  ((uint32_t)((x) & 0xff) * 0x01010101u)   /* 8-bit -> 32-bit kanał koloru */

/* ---------- stan ---------- */

struct seat {
	struct wl_list link;
	struct river_seat_v1 *rs;
	struct river_layer_shell_seat_v1 *ls;
	struct river_xkb_binding_v1 *binds[NKEYS];
	struct river_pointer_binding_v1 *pbinds[2];   /* Super+lewy, Super+prawy */
	bool binds_new;
};

static struct wl_display *display;
struct river_window_manager_v1 *wm;
struct wl_compositor *compositor;
struct wl_shm *shm;
struct wp_viewporter *viewporter;                  /* opcjonalne: ostry pasek przy skali 1,25 */
struct wp_fractional_scale_manager_v1 *fscale_mgr;
static struct river_xkb_bindings_v1 *xkb;
static struct river_layer_shell_v1 *layer_shell;
struct wl_list windows, outputs;
static struct wl_list seats;
struct win *focused;
static bool focus_dirty, close_focused, bars_dirty = true, running = true;
static bool anim_want;              /* po render: potrzebna kolejna klatka animacji */
static int ptr_x, ptr_y;
static bool ptr_known, warp;
static bool warp_focus;             /* po layoucie: kursor na środek okna z fokusem (FFM go nie odbierze) */

/* Opisy monitorów z wl_output: "AOC Q27G42XNE 1O0R9HA001780 (DP-9)". */
struct wlout {
	struct wl_list link;
	struct wl_output *wo;
	uint32_t name;
	char *desc, *conn;
};
static struct wl_list wlouts;
static struct win *hover;           /* okno pod kursorem (pointer_enter) */

/* Przeciąganie myszą: Super+lewy przesuwa, Super+prawy zmienia rozmiar.
 * Pływające — ruch i rozmiar; kafle — zamiana miejscami i proporcje podziału. */
enum { OP_NONE, OP_MOVE, OP_RESIZE };
static struct {
	int start;                      /* czeka na manage: OP_MOVE / OP_RESIZE */
	int kind;
	bool released;
	struct win *w;
	int dx, dy;
	int sx, sy, sw, sh;             /* geometria na starcie */
	struct node *hsplit, *vsplit;   /* podziały, których granicę ciągniemy */
	float hr0, vr0;
	int hw, vh;                     /* rozmiar tych podziałów w pikselach */
} op;

/* ---------- BSP ---------- */

static struct node *first_leaf(struct node *n)
{
	while (!n->win)
		n = n->a;
	return n;
}

/* Wstawia okno do drzewa: dzieli kafel okna `near` (albo pierwszy liść). */
static void bsp_insert(struct node **root, struct win *w, struct win *near)
{
	struct node *leaf = calloc(1, sizeof *leaf);
	leaf->win = w;
	w->leaf = leaf;
	if (!*root) {
		*root = leaf;
		return;
	}
	struct node *t = near ? near->leaf : first_leaf(*root);
	struct node *old = calloc(1, sizeof *old);
	*old = *t;                      /* stary liść schodzi piętro niżej */
	old->parent = t;
	old->win->leaf = old;
	t->win = NULL;                  /* t staje się węzłem wewnętrznym */
	t->a = old;
	t->b = leaf;
	t->side_by_side = t->w >= t->h; /* tniemy dłuższy bok */
	t->ratio = 0.5f;
	leaf->parent = t;
}

/* Wstawia okno obok okna t: podział w zadanym kierunku, nowe przed albo za t. */
static void bsp_insert_at(struct win *w, struct win *t, bool horiz, bool before)
{
	struct node *leaf = calloc(1, sizeof *leaf), *n = t->leaf;
	struct node *old = calloc(1, sizeof *old);
	leaf->win = w;
	w->leaf = leaf;
	*old = *n;                      /* t schodzi piętro niżej */
	old->parent = n;
	t->leaf = old;
	n->win = NULL;
	n->side_by_side = horiz;
	n->ratio = 0.5f;
	n->a = before ? leaf : old;
	n->b = before ? old : leaf;
	leaf->parent = n;
}

/* Usuwa okno; rodzeństwo zajmuje miejsce rodzica. Zwraca ten węzeł. */
static struct node *bsp_remove(struct node **root, struct win *w)
{
	struct node *leaf = w->leaf, *p = leaf->parent;
	free(leaf);
	w->leaf = NULL;
	if (!p) {
		*root = NULL;
		return NULL;
	}
	struct node *sib = p->a == leaf ? p->b : p->a;
	p->win = sib->win;
	p->a = sib->a;
	p->b = sib->b;
	p->side_by_side = sib->side_by_side;
	p->ratio = sib->ratio;
	if (p->win)
		p->win->leaf = p;
	else
		p->a->parent = p->b->parent = p;
	free(sib);
	return p;
}

static void layout(struct node *n, int x, int y, int w, int h)
{
	n->x = x; n->y = y; n->w = w; n->h = h;
	if (n->win) {
		int in = zen_on ? 0 : GAP / 2 + BORDER;
		n->win->x = x + in;
		n->win->y = y + in;
		n->win->w = w - 2 * in > 1 ? w - 2 * in : 1;
		n->win->h = h - 2 * in > 1 ? h - 2 * in : 1;
	} else if (n->side_by_side) {
		int aw = (int)(w * n->ratio);
		layout(n->a, x, y, aw, h);
		layout(n->b, x + aw, y, w - aw, h);
	} else {
		int ah = (int)(h * n->ratio);
		layout(n->a, x, y, w, ah);
		layout(n->b, x, y + ah, w, h - ah);
	}
}

/* ---------- okna, monitory, fokus ---------- */

static void set_focus(struct win *w)
{
	if (focused != w) {
		focused = w;
		focus_dirty = bars_dirty = true;   /* tytuł na pasku */
	}
}

static bool visible(struct win *w)
{
	return win_visible(w);
}

static struct win *first_on(struct output *o)
{
	struct node *r = o->roots[o->tag];
	return r ? first_leaf(r)->win : NULL;
}

/* Aktywny monitor: ten pod kursorem, potem ten z fokusem, potem pierwszy. */
static struct output *active_output(void)
{
	struct output *o;
	if (ptr_known)
		wl_list_for_each(o, &outputs, link)
			if (ptr_x >= o->x && ptr_x < o->x + o->w && ptr_y >= o->y && ptr_y < o->y + o->h)
				return o;
	if (focused && focused->out)
		return focused->out;
	return wl_list_empty(&outputs) ? NULL : wl_container_of(outputs.next, o, link);
}

struct output *focus_output(void)
{
	return focused && focused->out ? focused->out : active_output();
}

static struct output *neighbor(struct output *o, int dir)
{
	struct wl_list *l = dir > 0 ? o->link.next : o->link.prev;
	if (l == &outputs)              /* przeskok przez głowę listy */
		l = dir > 0 ? l->next : l->prev;
	struct output *n;
	return wl_container_of(l, n, link);
}

/* Dialog: ma rodzica, ma stały rozmiar albo jest na liście wyżej. */
static bool should_float(struct win *w)
{
	if (w->parent)
		return true;
	if (w->max_w > 0 && w->max_h > 0 && w->min_w == w->max_w && w->min_h == w->max_h)
		return true;
	for (int i = 0; w->app_id && float_app_ids[i]; i++)
		if (!strcmp(w->app_id, float_app_ids[i]))
			return true;
	return false;
}

static int clamp_hint(int v, int min, int max)
{
	if (max > 0 && v > max)
		v = max;
	if (min > 0 && v < min)
		v = min;
	return v;
}

/* Dialog: połowa szerokości i wysokości rodzica (bez rodzica — monitora),
 * w granicach min/max zgłoszonych przez okno. Stały rozmiar zostaje stały. */
static void dialog_size(struct win *w, int *pw, int *ph)
{
	int bw = w->out->aw, bh = w->out->ah;
	/* od pierwszego zwykłego okna w łańcuchu — dialog dialogu (np. wybór
	 * miejsca zapisu otwarty z okienka „zapisz/otwórz”) nie może skurczyć się
	 * do połowy małego okienka */
	struct win *p = w->parent;
	for (int i = 0; p && p->floating && i < 8; i++)
		p = p->parent;
	if (p && visible(p)) {
		bw = p->w;
		bh = p->h;
	}
	int dw = bw / 2 > 480 ? bw / 2 : 480, dh = bh / 2 > 360 ? bh / 2 : 360;
	if (dw > w->out->aw)
		dw = w->out->aw;
	if (dh > w->out->ah)
		dh = w->out->ah;
	*pw = clamp_hint(dw, w->min_w, w->max_w);
	*ph = clamp_hint(dh, w->min_h, w->max_h);
}

static void detach(struct win *w)
{
	bars_dirty = true;
	if (w->leaf)
		bsp_remove(&w->out->roots[w->tag], w);
	w->out = NULL;
}

/* Umieszcza okno na danym tagu monitora; obok fokusu, jeśli on tam jest. */
static void attach(struct win *w, struct output *o, int tag)
{
	struct win *near = focused && focused != w && focused->out == o &&
	                   focused->tag == tag && focused->leaf ? focused : NULL;
	w->out = o;
	w->tag = tag;
	bars_dirty = true;
	if (!w->floating)
		bsp_insert(&o->roots[tag], w, near);
}

/* ---------- przeciąganie myszą ---------- */

/* Podział, którego linia jest krawędzią okna: prawą/dolną (far) albo lewą/górną. */
static struct node *edge_split(struct win *w, bool horiz, bool far)
{
	struct node *child = w->leaf;
	for (struct node *p = child->parent; p; child = p, p = p->parent)
		if (p->side_by_side == horiz && (p->a == child) == far)
			return p;
	return NULL;
}

static struct win *tile_at(int x, int y, struct win *skip)
{
	struct win *w;
	wl_list_for_each(w, &windows, link) {
		if (w == skip || !w->leaf || !visible(w))
			continue;
		struct node *n = w->leaf;
		if (x >= n->x && x < n->x + n->w && y >= n->y && y < n->y + n->h)
			return w;
	}
	return NULL;
}

/* Zamiana dwóch kafli miejscami — także między monitorami i tagami. */
static void swap_wins(struct win *a, struct win *b)
{
	struct node *la = a->leaf, *lb = b->leaf;
	la->win = b;
	lb->win = a;
	a->leaf = lb;
	b->leaf = la;
	struct output *o = a->out;
	int tag = a->tag;
	a->out = b->out;
	a->tag = b->tag;
	b->out = o;
	b->tag = tag;
	bars_dirty = true;
}

static struct output *output_at(int x, int y)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		if (x >= o->x && x < o->x + o->w && y >= o->y && y < o->y + o->h)
			return o;
	return NULL;
}

/* Upuszczenie kafla w (x, y): środek innego kafla — zamiana, pas przy jego
 * krawędzi — wstawienie obok, pusta przestrzeń innego monitora — przeniesienie. */
static void drop_tile(struct win *w, int x, int y)
{
	struct win *t = tile_at(x, y, w);
	if (t) {
		struct node *n = t->leaf;
		float fx = (float)(x - n->x) / n->w, fy = (float)(y - n->y) / n->h;
		/* odległość od najbliższej krawędzi, 0 = na krawędzi, 0.5 = środek */
		float dl = fx, dr = 1 - fx, du = fy, dd = 1 - fy;
		float m = dl < dr ? dl : dr, mv = du < dd ? du : dd;
		if ((m < mv ? m : mv) >= 0.25f) {
			swap_wins(w, t);
			return;
		}
		struct output *to = t->out;
		int tag = t->tag;
		detach(w);                      /* t->leaf jest aktualizowany przez bsp_remove */
		w->out = to;
		w->tag = tag;
		if (m < mv)
			bsp_insert_at(w, t, true, dl < dr);
		else
			bsp_insert_at(w, t, false, du < dd);
		bars_dirty = true;
		return;
	}
	struct output *o = output_at(x, y);
	if (o && (o != w->out || !o->roots[o->tag] || o->tag != w->tag)) {
		detach(w);
		attach(w, o, o->tag);
	}
}

static float clamp_ratio(float r)
{
	return r < 0.1f ? 0.1f : r > 0.9f ? 0.9f : r;
}

static void set_dragging_all(struct output *o, bool on)
{
	struct win *w;
	wl_list_for_each(w, &windows, link)
		if (w->out == o && w->tag == o->tag)
			w->dragging = on;
}

/* Wołane z manage, przed layoutem. */
static void op_manage(void)
{
	if (wl_list_empty(&seats))
		return;
	struct seat *s = wl_container_of(seats.next, s, link);

	if (op.start && !op.kind) {
		int kind = op.start;
		struct win *w = hover;
		op.start = 0;
		if (w && visible(w)) {
			memset(&op, 0, sizeof op);
			op.kind = kind;
			op.w = w;
			op.sx = w->x;
			op.sy = w->y;
			op.sw = w->dw > 0 ? w->dw : w->w;
			op.sh = w->dh > 0 ? w->dh : w->h;
			if (kind == OP_RESIZE && w->leaf) {
				/* łapiemy krawędź bliższą kursorowi */
				op.hsplit = edge_split(w, true, ptr_x >= w->x + w->w / 2);
				op.vsplit = edge_split(w, false, ptr_y >= w->y + w->h / 2);
				if (op.hsplit) {
					op.hr0 = op.hsplit->ratio;
					op.hw = op.hsplit->w;
				}
				if (op.vsplit) {
					op.vr0 = op.vsplit->ratio;
					op.vh = op.vsplit->h;
				}
				set_dragging_all(w->out, true);     /* sąsiedzi bez dojazdu, za kursorem */
			} else {
				w->dragging = true;
			}
			if (kind == OP_RESIZE && w->floating)
				river_window_v1_inform_resize_start(w->rw);
			set_focus(w);
			river_seat_v1_op_start_pointer(s->rs);
			urd_cursor(kind == OP_MOVE ? "grabbing" : "se-resize");
		}
	}
	if (!op.kind)
		return;

	struct win *w = op.w;
	if (w && op.kind == OP_MOVE) {
		if (w->floating) {
			w->x = op.sx + op.dx;
			w->y = op.sy + op.dy;
			w->placed = true;
		} else {
			w->drag_dx = op.dx;
			w->drag_dy = op.dy;
		}
	} else if (w && w->floating) {
		int nw = clamp_hint(op.sw + op.dx, w->min_w, w->max_w);
		int nh = clamp_hint(op.sh + op.dy, w->min_h, w->max_h);
		if (nw < 80)
			nw = 80;
		if (nh < 50)
			nh = 50;
		if (nw != w->pw || nh != w->ph) {
			river_window_v1_propose_dimensions(w->rw, nw, nh);
			w->pw = nw;
			w->ph = nh;
		}
		w->placed = true;               /* lewy górny róg zostaje na miejscu */
	} else if (w) {
		if (op.hsplit && op.hw > 0)
			op.hsplit->ratio = clamp_ratio(op.hr0 + (float)op.dx / op.hw);
		if (op.vsplit && op.vh > 0)
			op.vsplit->ratio = clamp_ratio(op.vr0 + (float)op.dy / op.vh);
	}

	if (!op.released)
		return;
	if (w && op.kind == OP_MOVE && !w->floating) {
		w->drag_dx = w->drag_dy = 0;
		drop_tile(w, ptr_x, ptr_y);
	}
	if (w && op.kind == OP_RESIZE && w->floating)
		river_window_v1_inform_resize_end(w->rw);
	struct win *it;
	wl_list_for_each(it, &windows, link)
		it->dragging = false;
	if (w && !w->floating)
		w->tx = INT_MIN;                /* dojazd z miejsca upuszczenia do kafla */
	river_seat_v1_op_end(s->rs);
	urd_cursor("default");
	memset(&op, 0, sizeof op);
}

/* ---------- kierunki: Super+H/J/K/L ---------- */

static void win_rect(struct win *w, int *x, int *y, int *ww, int *wh)
{
	*x = w->x;
	*y = w->y;
	*ww = w->dw > 0 ? w->dw : w->w;
	*wh = w->dh > 0 ? w->dh : w->h;
}

/* Najbliższe widoczne okno w danym kierunku, na dowolnym monitorze:
 * jego środek musi leżeć za krawędzią okna z fokusem; wygrywa najmniejsza
 * odległość wzdłuż kierunku plus podwojone przesunięcie w poprzek. */
static struct win *dir_neighbor(struct win *from, int dir, bool tiles_only)
{
	int fx, fy, fw, fh;
	win_rect(from, &fx, &fy, &fw, &fh);
	int fcx = fx + fw / 2, fcy = fy + fh / 2;
	struct win *best = NULL, *w;
	long best_d = 0;
	wl_list_for_each(w, &windows, link) {
		if (w == from || !visible(w) || (tiles_only && !w->leaf))
			continue;
		int x, y, ww, wh;
		win_rect(w, &x, &y, &ww, &wh);
		int cx = x + ww / 2, cy = y + wh / 2;
		long along, across;
		switch (dir) {
		case LEFT:  if (cx >= fx) continue;      along = fx - cx;      across = cy - fcy; break;
		case RIGHT: if (cx <= fx + fw) continue; along = cx - fx - fw; across = cy - fcy; break;
		case UP:    if (cy >= fy) continue;      along = fy - cy;      across = cx - fcx; break;
		default:    if (cy <= fy + fh) continue; along = cy - fy - fh; across = cx - fcx; break;
		}
		long d = along + 2 * (across < 0 ? -across : across);
		if (!best || d < best_d) {
			best = w;
			best_d = d;
		}
	}
	return best;
}

static void center_pointer(struct output *o)
{
	ptr_x = o->x + o->w / 2;
	ptr_y = o->y + o->h / 2;
	ptr_known = warp = true;
}

/* ---------- akcje ---------- */

static void spawn(const char *const *cmd)
{
	if (fork() == 0) {
		setsid();
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		execvp(cmd[0], (char *const *)cmd);
		_exit(1);
	}
}

/* Fuzzel motywu na monitorze pod kursorem (bez --output wybiera sam);
 * help = ściąga ze skrótami w trybie --dmenu, czytana ze stdin. */
static void spawn_run(bool help)
{
	const char *argv[40];
	int n = 0;
	for (const char *const *a = THEME->run; *a && n < 30; a++)
		argv[n++] = *a;
	char opt[80], lines[32];
	struct output *o = active_output();   /* monitor pod kursorem — także pusty */
	struct wlout *wo;
	if (o)
		wl_list_for_each(wo, &wlouts, link)
			if (wo->name == o->wl_name && wo->conn) {
				snprintf(opt, sizeof opt, "--output=%s", wo->conn);
				argv[n++] = opt;
				break;
			}
	int nl = 0;
	if (help) {
		while (help_lines[nl])
			nl++;
		snprintf(lines, sizeof lines, "--lines=%d", nl);
		argv[n++] = "--dmenu";
		argv[n++] = "--prompt=SKRÓTY  ";
		argv[n++] = "--width=72";
		argv[n++] = lines;
	}
	argv[n] = NULL;
	int fds[2] = { -1, -1 };
	if (help && pipe(fds) < 0)
		return;
	pid_t pid = fork();
	if (pid == 0) {
		setsid();
		int fd = open("/dev/null", O_RDWR);
		if (fd >= 0) {
			dup2(help ? fds[0] : fd, STDIN_FILENO);
			dup2(fd, STDOUT_FILENO);
			dup2(fd, STDERR_FILENO);
			close(fd);
		}
		if (help) {
			close(fds[0]);
			close(fds[1]);
		}
		execvp(argv[0], (char *const *)argv);
		_exit(1);
	}
	if (!help)
		return;
	close(fds[0]);
	if (pid > 0)
		for (int i = 0; i < nl; i++)        /* kilka linii mieści się w buforze rury */
			if (write(fds[1], help_lines[i], strlen(help_lines[i])) < 0 || write(fds[1], "\n", 1) < 0)
				break;
	close(fds[1]);
}

/* ---------- akcje wołane też z paska ---------- */

void view_tag(struct output *o, int tag)
{
	if (o->tag == tag)
		return;
	anim_tag_switch(o, o->tag, tag);
	o->tag = tag;
	bars_dirty = true;
	set_focus(first_on(o));
	river_window_manager_v1_manage_dirty(wm);   /* z paska nie ma manage w drodze */
}

/* $XDG_x_HOME/norns/<name>, z domyślnym ~/<fallback> gdy zmienna pusta. */
static void xdg_path(char *buf, size_t n, const char *var, const char *fallback, const char *name)
{
	const char *base = getenv(var), *home = getenv("HOME");
	if (base && *base)
		snprintf(buf, n, "%s/norns/%s", base, name);
	else
		snprintf(buf, n, "%s/%s/norns/%s", home ? home : "", fallback, name);
}

/* Skrypt użytkownika dla aplikacji (foot itd.): ~/.config/norns/theme <nazwa>. */
static void run_theme_script(void)
{
	char path[512];
	xdg_path(path, sizeof path, "XDG_CONFIG_HOME", ".config", "theme");
	if (access(path, X_OK) == 0)
		spawn((const char *const[]){ path, THEME->name, NULL });
}

/* Wybrany motyw przeżywa restart: ~/.local/state/norns/theme. */
static void save_theme(void)
{
	char path[512];
	xdg_path(path, sizeof path, "XDG_STATE_HOME", ".local/state", "theme");
	char *slash = strrchr(path, '/');
	*slash = 0;
	mkdir(path, 0755);              /* ~/.local/state musi już istnieć — tworzymy tylko norns/ */
	*slash = '/';
	FILE *f = fopen(path, "w");
	if (f) {
		fprintf(f, "%s\n", THEME->name);
		fclose(f);
	}
}

static void load_theme(void)
{
	char path[512], name[32] = "";
	xdg_path(path, sizeof path, "XDG_STATE_HOME", ".local/state", "theme");
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	if (fscanf(f, "%31s", name) == 1)
		for (int i = 0; i < nthemes; i++)
			if (!strcmp(name, themes[i].name))
				theme_idx = i;
	fclose(f);
}

/* Menu wyłączania z paska. Wyłączenie i restart przez logind (systemctl). */
/* Aplikacje z zasobnika (Electron: Beeper itd.) rysują menu jako zwykłe nowe
 * okno. Okno, które pojawi się chwilę po prawym kliku w ikonkę, traktujemy
 * jak menu: pływa, ma własny rozmiar, stoi pod kursorem przy pasku. */
static struct { uint64_t t; int x, y; } popup;

void expect_popup(int x, int y)
{
	popup.t = now_ms();
	popup.x = x;
	popup.y = y;
}

void power_action(int i)
{
	static const char *const reboot[] = { "systemctl", "reboot", NULL };
	static const char *const poweroff[] = { "systemctl", "poweroff", NULL };
	if (i == 0)
		river_window_manager_v1_exit_session(wm);   /* koniec sesji rivera */
	else
		spawn(i == 1 ? reboot : poweroff);
}

static int theme_by_name(const char *name)
{
	for (int i = 0; name && i < nthemes; i++)
		if (!strcmp(name, themes[i].name))
			return i;
	return -1;
}

/* Ustawia motyw: ramki, pasek, tapeta, plus skrypt użytkownika dla aplikacji. */
static void set_theme(int i, bool notice)
{
	if (i < 0 || i == theme_idx)
		return;
	theme_idx = i;
	bars_dirty = true;
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		o->wall.dirty = true;
	save_theme();
	run_theme_script();
	if (notice)
		norns_notice("Motyw", THEME->label);
	river_window_manager_v1_manage_dirty(wm);
}

/* Następny motyw po kolei (Super+T, klik na pasku). */
void toggle_theme(void)
{
	set_theme((theme_idx + 1) % nthemes, true);
}

/* ---------- terminal wysuwany (Super+`) ---------- */

static struct win *drop_win;
static bool drop_spawning;

/* Terminal wysuwany — także zanim norns go oznaczy (po restarcie znamy tylko app_id). */
static bool is_drop(const struct win *w)
{
	return w->drop || (w->app_id && !strcmp(w->app_id, DROP_APP_ID));
}

/* Cel na monitorze: wyśrodkowany, nad dolną krawędzią. */
static void drop_place(struct win *w)
{
	struct output *o = w->out;
	w->w = DROP_W < o->aw * 9 / 10 ? DROP_W : o->aw * 9 / 10;
	w->h = DROP_H < o->ah * 9 / 10 ? DROP_H : o->ah * 9 / 10;
	w->x = o->ax + (o->aw - w->w) / 2;
	w->y = o->ay + o->ah - w->h - GAP;
}

/* Pod dolną krawędzią monitora (anim przycina okno do monitora). */
static int drop_offscreen_y(struct win *w)
{
	return w->out->y + w->out->h + BORDER + w->deco_r + 4;
}

/* Pokaż na monitorze o: wyjeżdża spod dolnej krawędzi. */
static void drop_show(struct win *w, struct output *o)
{
	int from = w->out && w->drop_t0 ? w->by : 0;    /* przerwane chowanie: jedzie z miejsca */
	bool same = w->out == o && w->drop_t0;
	if (w->out)
		detach(w);
	attach(w, o, o->tag);
	w->hidden = w->hiding = false;
	w->placed = true;
	drop_place(w);
	w->drop_from = same ? from : drop_offscreen_y(w);
	w->drop_to = w->y;
	w->drop_t0 = anim_on ? now_ms() : 0;
	if (anim_on)
		sfx_drop(true);
	set_focus(w);
}

static void drop_hide(struct win *w)
{
	struct output *o = w->out;
	struct win *n, *next = NULL;
	wl_list_for_each(n, &windows, link)
		if (n != w && n->out == o && visible(n)) {
			next = n;
			break;
		}
	set_focus(next);
	if (anim_on) {                      /* zjeżdża w dół, potem znika (render) */
		w->drop_from = w->by;
		w->drop_to = w->y = drop_offscreen_y(w);
		w->drop_t0 = now_ms();
		w->hiding = true;
		sfx_drop(false);
	} else {
		w->hidden = true;
		bars_dirty = true;
	}
}

static void drop_toggle(struct output *o)
{
	if (!o)
		return;
	if (!drop_win) {
		if (drop_spawning)
			return;
		const char *argv[24];
		int n = 0;
		argv[n++] = THEME->term[0];
		argv[n++] = "--app-id=" DROP_APP_ID;
		for (const char *const *a = THEME->term + 1; *a && n < 22; a++)
			argv[n++] = *a;
		argv[n] = NULL;
		drop_spawning = true;
		spawn(argv);
		return;
	}
	if (visible(drop_win) && drop_win->out == o && !drop_win->hiding)
		drop_hide(drop_win);
	else
		drop_show(drop_win, o);
}

/* ---------- zrzuty ekranu (grim, slurp, wl-copy) ---------- */

/* Nazwa złącza monitora (DP-9, eDP-1) — dla grim -o i norns-ctl status. */
static const char *conn_of(struct output *o)
{
	struct wlout *wo;
	wl_list_for_each(wo, &wlouts, link)
		if (wo->name == o->wl_name && wo->conn)
			return wo->conn;
	return "?";
}


/* 0 — monitor pod kursorem, 1 — zaznaczony obszar, 2 — okno z fokusem.
 * Plik jak w niri: ~/Pictures/Screenshots/Screenshot from RRRR-MM-DD GG-MM-SS.png,
 * do tego schowek, migawka i powiadomienie (verdandi). */
static void screenshot(int kind)
{
	char what[160];
	struct output *o = active_output();
	if (kind == 0) {
		if (!o)
			return;
		snprintf(what, sizeof what, "grim -o '%s'", conn_of(o));
	} else if (kind == 1) {
		snprintf(what, sizeof what, "g=$(slurp) || exit 0; grim -g \"$g\"");
	} else {
		if (!focused || !visible(focused))
			return;
		snprintf(what, sizeof what, "grim -g '%d,%d %dx%d'", focused->x, focused->y,
		         focused->dw > 0 ? focused->dw : focused->w, focused->dh > 0 ? focused->dh : focused->h);
	}
	char script[1024];
	snprintf(script, sizeof script,
	         "d=\"$(xdg-user-dir PICTURES 2>/dev/null || echo \"$HOME/Pictures\")/Screenshots\"; "
	         "mkdir -p \"$d\"; f=\"$d/Screenshot from $(date '+%%Y-%%m-%%d %%H-%%M-%%S').png\"; "
	         "%s \"$f\" || exit 1; %s'%s' & "
	         "wl-copy --type image/png < \"$f\" 2>/dev/null; "
	         "notify-send -a 'zrzut ekranu' 'Zapisano i skopiowano' \"$f\"",
	         what, zen_on ? "true " : "pw-play --volume 0.5 ", sfx_path("shutter.wav"));
	spawn((const char *const[]){ "/bin/sh", "-c", script, NULL });
}

/* ---------- wyłącznik (Super+Shift+Escape) ---------- */

/* Wybór w fuzzelu motywu; wybrana pozycja wraca przez norns-ctl power. */
static void power_menu(void)
{
	char script[2048], *p = script, *end = script + sizeof script;
	p += snprintf(p, end - p, "c=$(printf 'ZABLOKUJ\\nWYLOGUJ\\nRESTART\\nWYŁĄCZ\\n' |");
	for (const char *const *a = THEME->run; *a && p < end - 64; a++)
		p += snprintf(p, end - p, " '%s'", *a);       /* argumenty fuzzela bez apostrofów */
	struct output *o = active_output();
	if (o && p < end - 64)
		p += snprintf(p, end - p, " '--output=%s'", conn_of(o));
	snprintf(p, end - p, " --dmenu '--prompt=OFF  ' --lines=4 --width=24) || exit 0; "
	         "case \"$c\" in ZABLOKUJ) exec \"$HOME/.local/bin/norns-ctl\" lock;; WYLOGUJ) a=logout;; RESTART) a=reboot;; WYŁĄCZ) a=poweroff;; *) exit 0;; esac; "
	         "exec \"$HOME/.local/bin/norns-ctl\" power $a");
	spawn((const char *const[]){ "/bin/sh", "-c", script, NULL });
}

/* Zen: bez paska, przerw, ramek i poświaty; powiadomienia czekają, dźwięki milkną. */
bool zen_on;

static void zen_set(bool on)
{
	static int before = -1;         /* motyw sprzed zen */
	if (on == zen_on)
		return;
	zen_on = on;
	bars_dirty = true;
	int zt = theme_by_name(ZEN_THEME);
	if (on && zt >= 0) {
		before = theme_idx;
		set_theme(zt, false);
	} else if (!on && before >= 0 && theme_idx == zt) {
		set_theme(before, false);   /* zmieniony w trakcie zen — zostaje */
	}
	if (!on)
		notify_release();
	river_window_manager_v1_manage_dirty(wm);
}

/* Wołane z eventu pressed — zmieniamy tylko nasze dane, requesty idą w manage. */
static void run(const struct key *k)
{
	struct output *o = active_output(), *n;
	struct win *n_w;

	if (saver_on) {                         /* pierwszy skrót tylko budzi z wygaszacza */
		fprintf(stderr, "norns: wygaszacz — budzi skrót\n");
		fx_set_saver(false);
		return;
	}
	switch (k->act) {
	case SPAWN:
		if (!k->cmd && k->arg)
			spawn_run(false);
		else
			spawn(k->cmd ? k->cmd : THEME->term);
		break;
	case CLOSE:
		close_focused = true;
		break;
	case QUIT:
		river_window_manager_v1_stop(wm);
		break;
	case VIEW:
		if (o)
			view_tag(o, k->arg);
		break;
	case SEND_TAG:
		if (focused && focused->out && focused->tag != k->arg) {
			struct output *fo = focused->out;
			detach(focused);
			attach(focused, fo, k->arg);
		}
		break;
	case FOCUS_OUT:
		if (o && (n = neighbor(o, k->arg)) != o) {
			center_pointer(n);
			set_focus(first_on(n));
		}
		break;
	case SEND_OUT:
		if (focused && focused->out && (n = neighbor(focused->out, k->arg)) != focused->out) {
			detach(focused);
			attach(focused, n, n->tag);
		}
		break;
	case THEME_TOGGLE:
		toggle_theme();
		break;
	case FLOAT_TOGGLE:
		if (focused && focused->out && !focused->drop) {        /* terminal wysuwany zawsze pływa */
			struct output *fo = focused->out;
			int tag = focused->tag;
			detach(focused);
			focused->floating = !focused->floating;
			focused->float_pending = focused->floating ? 2 : 0;
			focused->pw = focused->ph = -1;     /* kafelek dostanie rozmiar od nowa */
			focused->tiled_dirty = true;
			attach(focused, fo, tag);
		}
		break;
	case FOCUS_DIR:
		if (!focused || !visible(focused)) {
			if (o)
				set_focus(first_on(o));
		} else if ((n_w = dir_neighbor(focused, k->arg, false))) {
			set_focus(n_w);
			warp_focus = true;
		}
		break;
	case SWAP_DIR:
		if (focused && focused->leaf && visible(focused) && (n_w = dir_neighbor(focused, k->arg, true))) {
			swap_wins(focused, n_w);
			focused->tx = INT_MIN;          /* dojazd na nowe miejsce */
			warp_focus = true;
		}
		break;
	case VOLUME:
		/* wpctl synchronicznie, żeby pasek od razu pokazał nową wartość.
		 * waitpid na konkretny pid — samo wait() przy SIGCHLD=SIG_IGN
		 * czekałoby na wszystkie dzieci, też na odpalone aplikacje. */
		{
			pid_t pid = fork();
			if (pid == 0) {
				execvp(k->cmd[0], (char *const *)k->cmd);
				_exit(1);
			}
			if (pid > 0)
				waitpid(pid, NULL, 0);
		}
		sensors_update_vol();
		urd_redraw_bars();
		break;
	case FULLSCREEN_TOGGLE:
		if (focused && !focused->drop)
			focused->fs_want = focused->fullscreen ? 2 : 1;
		break;
	case HELP:
		spawn_run(true);
		break;
	case ZEN_TOGGLE:
		zen_set(!zen_on);
		break;
	case CRT_TOGGLE:
		fx_set_crt(!crt_on);
		break;
	case ANIM_TOGGLE:
		anim_on = !anim_on;
		norns_notice("Animacje", anim_on ? "włączone" : "wyłączone");
		river_window_manager_v1_manage_dirty(wm);
		break;
	case SHOT:
		screenshot(k->arg);
		break;
	case POWER_MENU:
		power_menu();
		break;
	case LOCK:
		spawn(THEME->lock);         /* locker z config.h; river sam pilnuje ext-session-lock */
		break;
	case DROP_TOGGLE:
		drop_toggle(active_output());       /* monitor pod kursorem — też pusty */
		river_window_manager_v1_manage_dirty(wm);
		break;
	}
}

/* ---------- river_window_v1 ---------- */

static void win_closed(void *data, struct river_window_v1 *rw)
{
	struct win *w = data;
	fx_window_closed(w);                /* przed zmianą fokusu: kolor ramki */
	if (w == drop_win)
		drop_win = NULL;
	if (w->leaf) {
		struct node *p = bsp_remove(&w->out->roots[w->tag], w);
		if (focused == w)
			set_focus(p ? first_leaf(p)->win : NULL);
	} else if (focused == w) {
		set_focus(w->parent && visible(w->parent) ? w->parent : NULL);  /* dialog -> rodzic */
	}
	struct win *o;
	wl_list_for_each(o, &windows, link)
		if (o->parent == w)
			o->parent = NULL;
	bars_dirty = true;
	if (hover == w)
		hover = NULL;
	if (op.w == w) {                    /* zamknięte w trakcie przeciągania */
		op.w = NULL;
		op.released = true;
		river_window_manager_v1_manage_dirty(wm);
	}
	deco_fini(w);
	river_node_v1_destroy(w->rn);
	river_window_v1_destroy(rw);
	wl_list_remove(&w->link);
	free(w->title);
	free(w->app_id);
	free(w);
}

static void win_title(void *data, struct river_window_v1 *rw, const char *title)
{
	struct win *w = data;
	free(w->title);
	w->title = title ? strdup(title) : NULL;
	if (w == focused) {
		bars_dirty = true;
		river_window_manager_v1_manage_dirty(wm);   /* przerysuj tytuł na pasku */
	}
}

static void win_dimensions_hint(void *data, struct river_window_v1 *rw,
                                int32_t min_w, int32_t min_h, int32_t max_w, int32_t max_h)
{
	struct win *w = data;
	w->min_w = min_w;
	w->min_h = min_h;
	w->max_w = max_w;
	w->max_h = max_h;
}

static void win_dimensions(void *data, struct river_window_v1 *rw, int32_t width, int32_t height)
{
	struct win *w = data;
	w->dw = width;
	w->dh = height;
}

static void win_app_id(void *data, struct river_window_v1 *rw, const char *app_id)
{
	struct win *w = data;
	free(w->app_id);
	w->app_id = app_id ? strdup(app_id) : NULL;
}

static void win_parent(void *data, struct river_window_v1 *rw, struct river_window_v1 *parent)
{
	struct win *w = data;
	w->parent = parent ? river_window_v1_get_user_data(parent) : NULL;
}
static void win_decoration_hint(void *d, struct river_window_v1 *rw, uint32_t h) {}
static void win_pointer_move_requested(void *d, struct river_window_v1 *rw, struct river_seat_v1 *s) {}
static void win_pointer_resize_requested(void *d, struct river_window_v1 *rw, struct river_seat_v1 *s, uint32_t e) {}
static void win_show_window_menu_requested(void *d, struct river_window_v1 *rw, int32_t x, int32_t y) {}
static void win_maximize_requested(void *d, struct river_window_v1 *rw) {}
static void win_unmaximize_requested(void *d, struct river_window_v1 *rw) {}
static void win_fullscreen_requested(void *data, struct river_window_v1 *rw, struct river_output_v1 *o)
{
	struct win *w = data;
	w->fs_want = 1;
	w->fs_out = o ? river_output_v1_get_user_data(o) : NULL;
}

static void win_exit_fullscreen_requested(void *data, struct river_window_v1 *rw)
{
	struct win *w = data;
	w->fs_want = 2;
}
static void win_minimize_requested(void *d, struct river_window_v1 *rw) {}
static void win_unreliable_pid(void *d, struct river_window_v1 *rw, int32_t pid) {}
static void win_presentation_hint(void *d, struct river_window_v1 *rw, uint32_t h) {}
static void win_identifier(void *data, struct river_window_v1 *rw, const char *id)
{
	struct win *w = data;
	snprintf(w->ident, sizeof w->ident, "%s", id);
}
static void win_capture_sessions(void *d, struct river_window_v1 *rw, uint32_t n) {}
static void win_touch_move_requested(void *d, struct river_window_v1 *rw, struct river_seat_v1 *s, int32_t t) {}
static void win_touch_resize_requested(void *d, struct river_window_v1 *rw, struct river_seat_v1 *s, int32_t t, uint32_t e) {}

static const struct river_window_v1_listener win_listener = {
	.closed = win_closed,
	.dimensions_hint = win_dimensions_hint,
	.dimensions = win_dimensions,
	.app_id = win_app_id,
	.title = win_title,
	.parent = win_parent,
	.decoration_hint = win_decoration_hint,
	.pointer_move_requested = win_pointer_move_requested,
	.pointer_resize_requested = win_pointer_resize_requested,
	.show_window_menu_requested = win_show_window_menu_requested,
	.maximize_requested = win_maximize_requested,
	.unmaximize_requested = win_unmaximize_requested,
	.fullscreen_requested = win_fullscreen_requested,
	.exit_fullscreen_requested = win_exit_fullscreen_requested,
	.minimize_requested = win_minimize_requested,
	.unreliable_pid = win_unreliable_pid,
	.presentation_hint = win_presentation_hint,
	.identifier = win_identifier,
	.capture_sessions = win_capture_sessions,
	.touch_move_requested = win_touch_move_requested,
	.touch_resize_requested = win_touch_resize_requested,
};

/* ---------- river_output_v1 ---------- */

/* Monitor znika: jego okna odpinamy, manage przypnie je do innego. */
static void out_removed(void *data, struct river_output_v1 *ro)
{
	struct output *o = data;
	struct win *w;
	wl_list_for_each(w, &windows, link)
		if (w->out == o)
			detach(w);
	urd_output_fini(o);
	fx_output_fini(o);
	if (o->lo)
		river_layer_shell_output_v1_destroy(o->lo);
	river_output_v1_destroy(ro);
	wl_list_remove(&o->link);
	free(o);
}

static void out_position(void *data, struct river_output_v1 *ro, int32_t x, int32_t y)
{
	struct output *o = data;
	o->x = x;
	o->y = y;
}

static void out_dimensions(void *data, struct river_output_v1 *ro, int32_t w, int32_t h)
{
	struct output *o = data;
	o->w = w;
	o->h = h;
}

static void out_wl_output(void *data, struct river_output_v1 *ro, uint32_t name)
{
	struct output *o = data;
	o->wl_name = name;
}

/* Monitor pasujący do reguły: fragment opisu albo nazwy złącza. */
static struct output *rule_output(const char *match)
{
	struct output *o;
	struct wlout *wo;
	wl_list_for_each(o, &outputs, link)
		wl_list_for_each(wo, &wlouts, link)
			if (wo->name == o->wl_name &&
			    ((wo->desc && strstr(wo->desc, match)) || (wo->conn && strstr(wo->conn, match))))
				return o;
	return NULL;
}

static const struct rule *rule_for(struct win *w)
{
	for (size_t i = 0; w->app_id && i < NRULES; i++)
		if (!strcmp(w->app_id, rules[i].app_id))
			return &rules[i];
	return NULL;
}
static void out_capture_sessions(void *d, struct river_output_v1 *ro, uint32_t n) {}

static const struct river_output_v1_listener out_listener = {
	.removed = out_removed,
	.wl_output = out_wl_output,
	.position = out_position,
	.dimensions = out_dimensions,
	.capture_sessions = out_capture_sessions,
};

/* river_layer_shell_output_v1 — ile miejsca zostaje po panelach */
static void lo_non_exclusive_area(void *data, struct river_layer_shell_output_v1 *lo,
                                  int32_t x, int32_t y, int32_t w, int32_t h)
{
	struct output *o = data;
	o->ux = x;
	o->uy = y;
	o->uw = w;
	o->uh = h;
}

static const struct river_layer_shell_output_v1_listener lo_listener = {
	.non_exclusive_area = lo_non_exclusive_area,
};

/* ---------- norns-ctl: sterowanie przez D-Bus ---------- */

#define CTL_NAME  "io.github.pablofrelo.Norns"
#define CTL_PATH  "/io/github/pablofrelo/Norns"

/* Stan dla skryptów: motyw, tryby, a dla każdego monitora widoczny tag i zajęte. */
static void ctl_status(char *out, size_t n)
{
	int len = snprintf(out, n, "theme=%s anim=%s crt=%s saver=%s zen=%s focus=%s", THEME->name,
	                   anim_on ? "on" : "off", crt_on ? "on" : "off", saver_on ? "on" : "off",
	                   zen_on ? "on" : "off",
	                   focused && focused->app_id ? focused->app_id : "-");
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		char busy[NTAGS + 1] = { 0 };
		for (int i = 0; i < NTAGS; i++) {
			bool b = o->roots[i] != NULL;
			struct win *w;
			wl_list_for_each(w, &windows, link)
				if (w->out == o && w->tag == i && !w->hidden)
					b = true;
			busy[i] = b ? '1' + i : '-';
		}
		if (len < (int)n)
			len += snprintf(out + len, n - len, " %s:%d:%s", conn_of(o), o->tag + 1, busy);
	}
}

/* Polecenie tekstem → ta sama akcja co skrót. */
static const char *ctl_run(const char *cmd, char *out, size_t n)
{
	char verb[32] = "", arg[32] = "";
	sscanf(cmd, "%31s %31s", verb, arg);
	struct key k = { 0 };
	bool on = !strcmp(arg, "on"), off = !strcmp(arg, "off"), want;

	if (!strcmp(verb, "status")) {
		ctl_status(out, n);
		return out;
	} else if (!strcmp(verb, "view") || !strcmp(verb, "send")) {
		int t = atoi(arg);
		if (t < 1 || t > NTAGS)
			return "błąd: tag 1–9";
		k.act = verb[0] == 'v' ? VIEW : SEND_TAG;
		k.arg = t - 1;
	} else if (!strcmp(verb, "output") || !strcmp(verb, "send-output")) {
		if (strcmp(arg, "next") && strcmp(arg, "prev"))
			return "błąd: next albo prev";
		k.act = verb[0] == 'o' ? FOCUS_OUT : SEND_OUT;
		k.arg = !strcmp(arg, "next") ? 1 : -1;
	} else if (!strcmp(verb, "theme")) {
		if (!*arg || !strcmp(arg, "toggle"))
			toggle_theme();
		else if (theme_by_name(arg) < 0)
			return "błąd: toggle albo nazwa motywu (gruvbox, muthur, grafit, papier)";
		else
			set_theme(theme_by_name(arg), true);
		return "ok";
	} else if (!strcmp(verb, "crt")) {
		fx_set_crt(on ? true : off ? false : !crt_on);
		return "ok";
	} else if (!strcmp(verb, "zen")) {
		zen_set(on ? true : off ? false : !zen_on);
		return "ok";
	} else if (!strcmp(verb, "saver")) {
		fx_set_saver(on ? true : off ? false : !saver_on);
		return "ok";
	} else if (!strcmp(verb, "anim")) {
		want = on ? true : off ? false : !anim_on;
		if (want != anim_on)
			norns_notice("Animacje", want ? "włączone" : "wyłączone");
		anim_on = want;
		river_window_manager_v1_manage_dirty(wm);
		return "ok";
	} else if (!strcmp(verb, "power")) {
		int i = !strcmp(arg, "logout") ? 0 : !strcmp(arg, "reboot") ? 1 : !strcmp(arg, "poweroff") ? 2 : -1;
		if (i < 0)
			return "błąd: logout, reboot albo poweroff";
		power_action(i);
		return "ok";
	} else if (!strcmp(verb, "lock")) {
		k.act = LOCK;
	} else if (!strcmp(verb, "drop")) {
		k.act = DROP_TOGGLE;
	} else if (!strcmp(verb, "float")) {
		k.act = FLOAT_TOGGLE;
	} else if (!strcmp(verb, "fullscreen")) {
		k.act = FULLSCREEN_TOGGLE;
	} else if (!strcmp(verb, "close")) {
		k.act = CLOSE;
	} else if (!strcmp(verb, "help")) {
		k.act = HELP;
	} else if (!strcmp(verb, "launcher")) {
		k.act = SPAWN;
		k.arg = 1;
	} else {
		return "polecenia: status | view N | send N | output next|prev | send-output next|prev | "
		       "theme [toggle|gruvbox|muthur|grafit|papier] | anim [on|off] | crt [on|off] | saver [on|off] | zen [on|off] | drop | float | "
		       "lock | power logout|reboot|poweroff | "
		       "fullscreen | close | launcher | help";
	}
	run(&k);
	river_window_manager_v1_manage_dirty(wm);
	return "ok";
}

static int m_ctl(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *cmd;
	char out[512];
	if (sd_bus_message_read(m, "s", &cmd) < 0)
		return -EINVAL;
	return sd_bus_reply_method_return(m, "s", ctl_run(cmd, out, sizeof out));
}

static const sd_bus_vtable ctl_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Run", "s", "s", m_ctl, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_VTABLE_END,
};

void ctl_init(sd_bus *bus)
{
	if (sd_bus_add_object_vtable(bus, NULL, CTL_PATH, CTL_NAME, ctl_vtable, NULL) < 0 ||
	    sd_bus_request_name(bus, CTL_NAME, 0) < 0)
		fprintf(stderr, "norns: norns-ctl niedostępny (działa drugi norns?)\n");
}

/* ---------- klawisze ---------- */

static void bind_pressed(void *data, struct river_xkb_binding_v1 *xb)
{
	run(data);
}

static void bind_released(void *d, struct river_xkb_binding_v1 *xb) {}
static void bind_stop_repeat(void *d, struct river_xkb_binding_v1 *xb) {}

static const struct river_xkb_binding_v1_listener bind_listener = {
	.pressed = bind_pressed,
	.released = bind_released,
	.stop_repeat = bind_stop_repeat,
};

/* ---------- przyciski myszy z Super ---------- */

static void pbind_pressed(void *data, struct river_pointer_binding_v1 *pb)
{
	if (!op.kind)
		op.start = (int)(intptr_t)data;     /* okno ustalimy w manage, ze świeżą pozycją */
}

static void pbind_released(void *d, struct river_pointer_binding_v1 *pb) {}

static const struct river_pointer_binding_v1_listener pbind_listener = {
	.pressed = pbind_pressed,
	.released = pbind_released,
};

/* ---------- river_seat_v1 ---------- */

static void seat_removed(void *data, struct river_seat_v1 *rs)
{
	struct seat *s = data;
	for (size_t i = 0; i < NKEYS; i++)
		river_xkb_binding_v1_destroy(s->binds[i]);
	for (int i = 0; i < 2; i++)
		river_pointer_binding_v1_destroy(s->pbinds[i]);
	if (s->ls)
		river_layer_shell_seat_v1_destroy(s->ls);
	river_seat_v1_destroy(rs);
	wl_list_remove(&s->link);
	free(s);
}

/* focus-follows-mouse: najechanie = fokus */
static void seat_pointer_enter(void *d, struct river_seat_v1 *rs, struct river_window_v1 *rw)
{
	hover = river_window_v1_get_user_data(rw);
	set_focus(river_window_v1_get_user_data(rw));
}

static void seat_window_interaction(void *d, struct river_seat_v1 *rs, struct river_window_v1 *rw)
{
	set_focus(river_window_v1_get_user_data(rw));
}

static void seat_pointer_position(void *d, struct river_seat_v1 *rs, int32_t x, int32_t y)
{
	/* ruch myszy budzi z wygaszacza — liczony od pierwszej pozycji po włączeniu,
	 * bo ptr_x/y bywa przesunięty przez norns (kursor za fokusem) */
	static int ax, ay;
	static bool anchored;
	if (!saver_on) {
		anchored = false;
	} else if (!anchored) {
		ax = x;
		ay = y;
		anchored = true;
	} else if (abs(x - ax) > 8 || abs(y - ay) > 8) {
		fprintf(stderr, "norns: wygaszacz — budzi mysz\n");
		fx_set_saver(false);
	}
	ptr_x = x;
	ptr_y = y;
	ptr_known = true;
}

static void seat_wl_seat(void *d, struct river_seat_v1 *rs, uint32_t name) {}
static void seat_pointer_leave(void *d, struct river_seat_v1 *rs)
{
	hover = NULL;
}
static void seat_shell_surface_interaction(void *d, struct river_seat_v1 *rs, struct river_shell_surface_v1 *ss)
{
	if (saver_on && now_ms() > saver_since + 500) {     /* klik w wygaszacz */
		fprintf(stderr, "norns: wygaszacz — budzi klik\n");
		fx_set_saver(false);
	}
}
static void seat_op_delta(void *d, struct river_seat_v1 *rs, int32_t dx, int32_t dy)
{
	op.dx = dx;
	op.dy = dy;
}

static void seat_op_release(void *d, struct river_seat_v1 *rs)
{
	op.released = true;
}
static void seat_op_delta_touch(void *d, struct river_seat_v1 *rs, int32_t t, int32_t dx, int32_t dy) {}
static void seat_op_release_touch(void *d, struct river_seat_v1 *rs, int32_t t) {}
static void seat_op_cancel_touch(void *d, struct river_seat_v1 *rs, int32_t t) {}

static const struct river_seat_v1_listener seat_listener = {
	.removed = seat_removed,
	.wl_seat = seat_wl_seat,
	.pointer_enter = seat_pointer_enter,
	.pointer_leave = seat_pointer_leave,
	.window_interaction = seat_window_interaction,
	.shell_surface_interaction = seat_shell_surface_interaction,
	.op_delta = seat_op_delta,
	.op_release = seat_op_release,
	.pointer_position = seat_pointer_position,
	.op_delta_touch = seat_op_delta_touch,
	.op_release_touch = seat_op_release_touch,
	.op_cancel_touch = seat_op_cancel_touch,
};

/* river_layer_shell_seat_v1 — launcher/panel zabrał i oddał klawiaturę */
static void ls_focus_exclusive(void *d, struct river_layer_shell_seat_v1 *ls) {}
static void ls_focus_non_exclusive(void *d, struct river_layer_shell_seat_v1 *ls) {}

static void ls_focus_none(void *d, struct river_layer_shell_seat_v1 *ls)
{
	focus_dirty = true;     /* oddaj fokus oknu, które go miało */
}

static const struct river_layer_shell_seat_v1_listener ls_listener = {
	.focus_exclusive = ls_focus_exclusive,
	.focus_non_exclusive = ls_focus_non_exclusive,
	.focus_none = ls_focus_none,
};

/* ---------- river_window_manager_v1 ---------- */

static void wm_unavailable(void *d, struct river_window_manager_v1 *m)
{
	fprintf(stderr, "norns: window management niedostępny (działa już inny WM?)\n");
	exit(1);
}

static void wm_finished(void *d, struct river_window_manager_v1 *m)
{
	running = false;
}

/* ---------- zapamiętany układ (przeżywa restart norns, nie rivera) ---------- */
/*
 * Po każdej sekwencji manage norns zapisuje układ do $XDG_RUNTIME_DIR/norns/layout:
 * widoczny tag każdego monitora, drzewa BSP z proporcjami, pływające okna z
 * pozycją. Okna rozpoznaje po identyfikatorze od rivera, który się nie zmienia,
 * dopóki okno żyje. Po restarcie norns (run.sh, powrót z rhine) okna wracają na
 * swoje miejsca; okna, których już nie ma, po prostu wypadają z drzew.
 */

enum { RESTORE_NONE, RESTORE_PENDING, RESTORE_DONE };
static int restore_state;
static uint64_t restore_deadline;
static char *saved_layout;          /* treść pliku z poprzedniego uruchomienia */
static char *last_written;

static const char *desc_of(struct output *o)
{
	struct wlout *wo;
	wl_list_for_each(wo, &wlouts, link)
		if (wo->name == o->wl_name)
			return wo->desc;
	return NULL;
}

static void layout_path(char *buf, size_t n)
{
	const char *rt = getenv("XDG_RUNTIME_DIR");
	snprintf(buf, n, "%s/norns/layout", rt && *rt ? rt : "/tmp");
}

static void layout_load(void)
{
	char path[300];
	layout_path(path, sizeof path);
	FILE *f = fopen(path, "r");
	if (!f)
		return;
	size_t cap = 1 << 16, len = fread(saved_layout = malloc(cap), 1, cap - 1, f);
	fclose(f);
	saved_layout[len] = 0;
	restore_state = RESTORE_PENDING;
	restore_deadline = now_ms() + 2000;     /* nie czekamy w nieskończoność na opisy monitorów */
}

/* Czy okno jest w zapisanym układzie (wtedy czeka na odtworzenie). */
static bool layout_knows(struct win *w)
{
	if (!saved_layout || !*w->ident)
		return false;
	char key[48];
	snprintf(key, sizeof key, "\t%s\t", w->ident);
	char key2[48];
	snprintf(key2, sizeof key2, "\t%s\n", w->ident);
	return strstr(saved_layout, key) || strstr(saved_layout, key2);
}

static struct win *unplaced_by_ident(const char *id)
{
	struct win *w;
	wl_list_for_each(w, &windows, link)
		if (!w->out && *w->ident && !strcmp(w->ident, id))
			return w;
	return NULL;
}

static void tree_write(struct node *n, char **p, char *end)
{
	if (*p >= end - 64)
		return;
	if (n->win)
		*p += snprintf(*p, end - *p, "\tL\t%s", *n->win->ident && !is_drop(n->win) ? n->win->ident : "-");
	else {
		*p += snprintf(*p, end - *p, "\tN\t%d\t%.3f", n->side_by_side, n->ratio);
		tree_write(n->a, p, end);
		tree_write(n->b, p, end);
	}
}

/* Odbudowa drzewa z tokenów; liście bez okna wypadają, rodzic z jednym dzieckiem znika. */
static struct node *tree_read(char **tok, struct output *o, int tag)
{
	char *t = strsep(tok, "\t");
	if (!t)
		return NULL;
	if (!strcmp(t, "L")) {
		char *id = strsep(tok, "\t");
		struct win *w = id ? unplaced_by_ident(id) : NULL;
		if (!w || is_drop(w))
			return NULL;                            /* terminal wysuwany nigdy nie jest kaflem */
		struct node *leaf = calloc(1, sizeof *leaf);
		leaf->win = w;
		w->leaf = leaf;
		w->out = o;
		w->tag = tag;
		w->floating = false;
		return leaf;
	}
	if (strcmp(t, "N"))
		return NULL;
	char *sbs = strsep(tok, "\t"), *ratio = strsep(tok, "\t");
	struct node *a = tree_read(tok, o, tag), *b = tree_read(tok, o, tag);
	if (!a || !b)
		return a ? a : b;
	struct node *n = calloc(1, sizeof *n);
	n->side_by_side = sbs && *sbs == '1';
	n->ratio = ratio ? strtof(ratio, NULL) : 0.5f;
	if (n->ratio < 0.1f || n->ratio > 0.9f)
		n->ratio = 0.5f;
	n->a = a;
	n->b = b;
	a->parent = b->parent = n;
	return n;
}

static struct output *output_by_desc(const char *desc)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		const char *d = desc_of(o);
		if (d && !strcmp(d, desc))
			return o;
	}
	return NULL;
}

static bool outputs_described(void)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		if (!desc_of(o))
			return false;
	return !wl_list_empty(&outputs);
}

static void layout_restore(void)
{
	restore_state = RESTORE_DONE;
	struct win *focus = NULL;
	char *copy = strdup(saved_layout), *line, *rest = copy;
	while ((line = strsep(&rest, "\n"))) {
		char *tok = line, *kind = strsep(&tok, "\t");
		if (!kind || !tok)
			continue;
		if (!strcmp(kind, "O")) {                       /* O  opis  tag */
			char *desc = strsep(&tok, "\t"), *tag = strsep(&tok, "\t");
			struct output *o = desc ? output_by_desc(desc) : NULL;
			if (o && tag && atoi(tag) >= 0 && atoi(tag) < NTAGS)
				o->tag = atoi(tag);
		} else if (!strcmp(kind, "T")) {                /* T  opis  tag  drzewo */
			char *desc = strsep(&tok, "\t"), *tag = strsep(&tok, "\t");
			struct output *o = desc ? output_by_desc(desc) : NULL;
			int t = tag ? atoi(tag) : -1;
			if (!o || t < 0 || t >= NTAGS || o->roots[t])
				continue;
			o->roots[t] = tree_read(&tok, o, t);
		} else if (!strcmp(kind, "F")) {                /* F  id  opis  tag  x  y  (względem monitora) */
			char *id = strsep(&tok, "\t"), *desc = strsep(&tok, "\t"), *tag = strsep(&tok, "\t");
			char *x = strsep(&tok, "\t"), *y = strsep(&tok, "\t");
			struct win *w = id ? unplaced_by_ident(id) : NULL;
			struct output *o = desc ? output_by_desc(desc) : NULL;
			if (!w || !o || !tag || !x || !y || is_drop(w))
				continue;
			w->floating = w->placed = true;
			attach(w, o, atoi(tag) >= 0 && atoi(tag) < NTAGS ? atoi(tag) : o->tag);
			w->x = o->x + atoi(x);
			w->y = o->y + atoi(y);
		} else if (!strcmp(kind, "X")) {                /* X  id okna z fokusem */
			struct win *w;
			wl_list_for_each(w, &windows, link)
				if (*w->ident && !strcmp(w->ident, tok))
					focus = w;
		}
	}
	free(copy);
	struct win *w;
	int n = 0;
	wl_list_for_each(w, &windows, link) {
		if (!w->fresh || !w->out)
			continue;                                   /* reszta (np. monitor odpięty) — normalnie */
		w->float_pending = 0;
		w->tiled_dirty = true;
		w->fresh = false;
		river_window_v1_use_ssd(w->rw);
		n++;
	}
	if (focus && focus->out)
		set_focus(focus);
	bars_dirty = true;
	fprintf(stderr, "norns: odtworzony układ — %d okien na swoich miejscach\n", n);
}

static void layout_save(void)
{
	if (restore_state == RESTORE_PENDING)
		return;                                         /* nie nadpisujemy, póki nie odtworzone */
	static char buf[1 << 16];
	char *p = buf, *end = buf + sizeof buf;
	struct output *o;
	wl_list_for_each(o, &outputs, link) {
		const char *d = desc_of(o);
		if (!d)
			continue;
		p += snprintf(p, end - p, "O\t%s\t%d\n", d, o->tag);
		for (int t = 0; t < NTAGS && p < end - 256; t++) {
			if (!o->roots[t])
				continue;
			p += snprintf(p, end - p, "T\t%s\t%d", d, t);
			tree_write(o->roots[t], &p, end);
			p += snprintf(p, end - p, "\n");
		}
	}
	struct win *w;
	wl_list_for_each(w, &windows, link) {
		if (!w->out || !w->floating || w->drop || !*w->ident || p >= end - 256)
			continue;
		const char *d = desc_of(w->out);
		if (d)
			p += snprintf(p, end - p, "F\t%s\t%s\t%d\t%d\t%d\n", w->ident, d, w->tag,
			              w->x - w->out->x, w->y - w->out->y);
	}
	if (focused && *focused->ident && p < end - 64)
		p += snprintf(p, end - p, "X\t%s\n", focused->ident);
	if (last_written && !strcmp(last_written, buf))
		return;
	char path[300], tmp[310];
	layout_path(path, sizeof path);
	snprintf(tmp, sizeof tmp, "%s.tmp", path);
	FILE *f = fopen(tmp, "w");
	if (!f)
		return;
	fputs(buf, f);
	if (fclose(f) == 0 && rename(tmp, path) == 0) {
		free(last_written);
		last_written = strdup(buf);
	}
}

static void wm_manage_start(void *d, struct river_window_manager_v1 *m)
{
	struct output *o, *act = active_output();
	struct win *w;
	struct seat *s;

	/* po restarcie norns: okna z zapisanego układu wracają na swoje miejsca */
	if (restore_state == RESTORE_PENDING && (outputs_described() || now_ms() > restore_deadline))
		layout_restore();

	/* nowe i osierocone okna lądują na aktywnym monitorze */
	wl_list_for_each(w, &windows, link) {
		if (w->out || !act)
			continue;
		if (restore_state == RESTORE_PENDING && w->fresh && layout_knows(w))
			continue;                                   /* czeka na opisy monitorów */
		if (w->fresh) {
			w->floating = should_float(w);
			w->float_pending = w->floating ? 1 : 0;
			w->tiled_dirty = true;
			if (w->app_id && !strcmp(w->app_id, DROP_APP_ID) && !drop_win) {   /* terminal wysuwany */
				w->floating = w->drop = true;
				w->float_pending = 0;
				drop_win = w;
				drop_spawning = false;
				w->fresh = false;
				river_window_v1_use_ssd(w->rw);
				drop_show(w, act);
				continue;
			}
			if (popup.t && now_ms() - popup.t < 1500) {    /* menu z zasobnika */
				w->floating = true;
				w->float_pending = 3;               /* niech samo wybierze rozmiar */
				w->placed = true;
				w->x = popup.x;
				w->y = popup.y;
				popup.t = 0;
			}
		}
		/* dialog ląduje tam, gdzie jego rodzic; nowe okno — wg reguł, jeśli monitor jest */
		struct output *to = act;
		int tag = act->tag;
		const struct rule *r;
		if (w->floating && w->parent && w->parent->out) {
			to = w->parent->out;
			tag = w->parent->tag;
		} else if (w->fresh && w->float_pending != 3 && (r = rule_for(w))) {
			struct output *ro = rule_output(r->output);
			if (ro) {
				to = ro;
				tag = r->tag > 0 ? r->tag - 1 : ro->tag;
			}
		}
		attach(w, to, tag);
		if (w->fresh) {
			/* do /tmp/norns.log: przydatne przy pisaniu reguł i szukaniu zgubionych okien */
			fprintf(stderr, "norns: nowe okno app_id=%s rodzic=%s %s, min %dx%d max %dx%d\n",
			        w->app_id ? w->app_id : "?",
			        w->parent && w->parent->app_id ? w->parent->app_id : "-",
			        w->floating ? "pływa" : "kafel", w->min_w, w->min_h, w->max_w, w->max_h);
			w->fresh = false;
			river_window_v1_use_ssd(w->rw);
			set_focus(w);
		}
	}

	wl_list_for_each(w, &windows, link) {
		if (!w->tiled_dirty)
			continue;
		w->tiled_dirty = false;
		river_window_v1_set_tiled(w->rw, w->floating && !w->drop ? RIVER_WINDOW_V1_EDGES_NONE :
		                          RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		                          RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);
	}

	if (close_focused && focused)
		river_window_v1_close(focused->rw);
	close_focused = false;

	op_manage();

	/* pełny ekran: river sam ustawia rozmiar i pozycję; po wyjściu wracamy do kafla */
	wl_list_for_each(w, &windows, link) {
		if (!w->fs_want || !w->out)
			continue;
		if (w->fs_want == 1 && !w->fullscreen) {
			struct output *fo = w->fs_out ? w->fs_out : w->out;
			if (fo != w->out) {             /* aplikacja chce inny monitor — przenosimy okno */
				detach(w);
				attach(w, fo, fo->tag);
			}
			river_window_v1_fullscreen(w->rw, fo->ro);
			river_window_v1_inform_fullscreen(w->rw);
			w->fullscreen = true;
			set_focus(w);
		} else if (w->fs_want == 2 && w->fullscreen) {
			river_window_v1_exit_fullscreen(w->rw);
			river_window_v1_inform_not_fullscreen(w->rw);
			w->fullscreen = false;
			w->pw = w->ph = -1;             /* propose w tej samej sekwencji (frame perfect) */
			if (w->floating && w->w > 0)
				river_window_v1_propose_dimensions(w->rw, w->w, w->h);
		}
		w->fs_want = 0;
		w->fs_out = NULL;
	}

	/* fokus na niewidocznym oknie (np. wysłanym na inny tag) -> coś widocznego */
	if (act && (!focused || !visible(focused)))
		set_focus(first_on(act));

	int bh = urd_bar_height(), g = zen_on ? 0 : GAP / 2;
	wl_list_for_each(o, &outputs, link) {
		/* obszar: bez cudzych paneli (layer shell), minus nasz pasek u góry */
		bool u = o->uw > 0 && o->uh > 0;
		int ax = u ? o->ux : o->x, ay = u ? o->uy : o->y;
		int aw = u ? o->uw : o->w, ah = u ? o->uh : o->h;

		o->bar.x = ax;
		o->bar.y = ay;
		o->bar.w = aw;
		o->bar.h = bh;
		if (bars_dirty)
			o->bar.dirty = true;
		o->wall.x = o->x;
		o->wall.y = o->y;
		o->wall.w = o->w;
		o->wall.h = o->h;

		o->ax = ax;
		o->ay = ay + (zen_on ? 0 : bh);     /* zen: pasek schowany pod tapetą */
		o->aw = aw;
		o->ah = ah - (zen_on ? 0 : bh);

		struct node *r = o->roots[o->tag];
		if (r)
			layout(r, o->ax + g, o->ay + g, o->aw - 2 * g, o->ah - 2 * g);
	}
	bars_dirty = false;
	if (warp_focus && focused && visible(focused)) {
		ptr_x = focused->x + focused->w / 2;
		ptr_y = focused->y + focused->h / 2;
		ptr_known = warp = true;
	}
	warp_focus = false;

	wl_list_for_each(w, &windows, link) {
		if (!visible(w))
			continue;
		if (!w->floating) {
			if (w->w != w->pw || w->h != w->ph) {  /* każdy propose to reconfigure okna */
				river_window_v1_propose_dimensions(w->rw, w->w, w->h);
				w->pw = w->w;
				w->ph = w->h;
			}
		} else if (w->drop) {
			/* rozmiar zawsze z bieżącego obszaru: po restarcie norns pierwsza sekwencja
			 * nie zna jeszcze obszaru, a okno ma rozmiar po poprzednim WM (np. cały ekran) */
			int aw = w->out->aw > 0 ? w->out->aw : w->out->w, ah = w->out->ah > 0 ? w->out->ah : w->out->h;
			w->w = DROP_W < aw * 9 / 10 ? DROP_W : aw * 9 / 10;
			w->h = DROP_H < ah * 9 / 10 ? DROP_H : ah * 9 / 10;
			if (w->w > 0 && w->h > 0 && (w->w != w->pw || w->h != w->ph)) {
				river_window_v1_propose_dimensions(w->rw, w->w, w->h);
				w->pw = w->w;
				w->ph = w->h;
			}
		} else if (w->float_pending == 1) {                  /* dialog: pół na pół rodzica */
			int pw, ph;
			dialog_size(w, &pw, &ph);
			river_window_v1_propose_dimensions(w->rw, pw, ph);
		}
		else if (w->float_pending == 2)                    /* Super+F: 60% obszaru */
			river_window_v1_propose_dimensions(w->rw, w->out->aw * 3 / 5, w->out->ah * 3 / 5);
		else if (w->float_pending == 3)                    /* menu: własny rozmiar */
			river_window_v1_propose_dimensions(w->rw, 0, 0);
		if (w->floating)
			w->float_pending = 0;
	}

	wl_list_for_each(s, &seats, link) {
		if (s->binds_new) {
			for (size_t i = 0; i < NKEYS; i++)
				river_xkb_binding_v1_enable(s->binds[i]);
			for (int i = 0; i < 2; i++)
				river_pointer_binding_v1_enable(s->pbinds[i]);
			s->binds_new = false;
		}
		if (focus_dirty) {
			if (focused)
				river_seat_v1_focus_window(s->rs, focused->rw);
			else
				river_seat_v1_clear_focus(s->rs);
		}
		if (warp && river_seat_v1_get_version(s->rs) >= RIVER_SEAT_V1_POINTER_WARP_SINCE_VERSION)
			river_seat_v1_pointer_warp(s->rs, ptr_x, ptr_y);
	}
	focus_dirty = warp = false;

	layout_save();
	river_window_manager_v1_manage_finish(wm);
}

static void wm_render_start(void *d, struct river_window_manager_v1 *m)
{
	uint64_t now = now_ms();
	struct win *w;
	wl_list_for_each(w, &windows, link) {
		if (visible(w) && w->floating && !w->placed) {
			/* wyśrodkuj nad rodzicem (jeśli widoczny), inaczej nad obszarem monitora */
			int ax = w->out->ax, ay = w->out->ay, aw = w->out->aw, ah = w->out->ah;
			if (w->parent && visible(w->parent)) {
				ax = w->parent->x;
				ay = w->parent->y;
				aw = w->parent->w;
				ah = w->parent->h;
			}
			w->w = w->dw;
			w->h = w->dh;
			w->x = ax + (aw - w->w) / 2;
			w->y = ay + (ah - w->h) / 2;
		}
		if (w->drop && visible(w) && w->dw > 0) {
			/* środek wg prawdziwego rozmiaru: foot zaokrągla okno do pełnych znaków */
			struct output *o = w->out;
			w->x = o->ax + (o->aw - w->dw) / 2;
			if (!w->hiding)
				w->y = w->drop_to = o->ay + o->ah - w->dh - GAP;
		}
		if (w->drop && w->hiding && !w->drop_t0) {
			w->hiding = false;          /* odjechał — chowamy */
			w->hidden = true;
			bars_dirty = true;
		}
		if (visible(w) && w->floating && w->placed && !w->drop) {
			w->w = w->dw;
			w->h = w->dh;
			/* nie wychodzi za monitor (menu z zasobnika przy prawej krawędzi) */
			struct output *o = w->out;
			if (w->x + w->w + BORDER > o->x + o->w)
				w->x = o->x + o->w - w->w - BORDER;
			if (w->y + w->h + BORDER > o->y + o->h)
				w->y = o->y + o->h - w->h - BORDER;
			if (w->x < o->x + BORDER)
				w->x = o->x + BORDER;
			if (w->y < o->y + BORDER)
				w->y = o->y + BORDER;
		}
		if (visible(w))
			deco_render(w);             /* przed anim: margines poświaty wchodzi w przycięcie */
		anim_render_window(w, now);
		if (!w->shown)
			continue;
		uint32_t c = w == focused ? THEME->focus : THEME->unfocus;
		if (w->floating || (w == op.w && op.kind == OP_MOVE))
			river_node_v1_place_top(w->rn);    /* nad kafelkami (pasek i tak wyżej) */
		river_window_v1_set_borders(w->rw, zen_on ? RIVER_WINDOW_V1_EDGES_NONE :
		                            RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		                            RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
		                            BORDER, C8(c >> 16), C8(c >> 8), C8(c), 0xffffffffu);
	}

	if (focused && focused->floating && visible(focused))
		river_node_v1_place_top(focused->rn);  /* pływające z fokusem na wierzchu */

	struct output *o;
	wl_list_for_each(o, &outputs, link)
		urd_render(o);

	/* pełny ekran nad paskiem (urd kładzie pasek na wierzch) */
	wl_list_for_each(w, &windows, link)
		if (w->fullscreen && visible(w))
			river_node_v1_place_top(w->rn);
	wl_list_for_each(w, &windows, link)     /* dialogi okna pełnoekranowego nad nim */
		if (w->floating && w->parent && w->parent->fullscreen && visible(w))
			river_node_v1_place_top(w->rn);


	fx_render();                            /* warstwa efektów na samej górze */

	anim_want = anim_busy(now);
	river_window_manager_v1_render_finish(wm);
}

static void wm_window(void *d, struct river_window_manager_v1 *m, struct river_window_v1 *rw)
{
	struct win *w = calloc(1, sizeof *w);
	w->rw = rw;
	w->rn = river_window_v1_get_node(rw);
	w->fresh = true;
	w->pw = w->ph = -1;
	deco_init(w);
	river_window_v1_add_listener(rw, &win_listener, w);
	wl_list_insert(windows.prev, &w->link);   /* na koniec listy */
}

static void wm_output(void *d, struct river_window_manager_v1 *m, struct river_output_v1 *ro)
{
	struct output *o = calloc(1, sizeof *o);
	o->ro = ro;
	river_output_v1_add_listener(ro, &out_listener, o);
	urd_output_init(o);
	if (layer_shell) {
		o->lo = river_layer_shell_v1_get_output(layer_shell, ro);
		river_layer_shell_output_v1_add_listener(o->lo, &lo_listener, o);
	}
	wl_list_insert(outputs.prev, &o->link);
}

static void wm_seat(void *d, struct river_window_manager_v1 *m, struct river_seat_v1 *rs)
{
	struct seat *s = calloc(1, sizeof *s);
	s->rs = rs;
	river_seat_v1_add_listener(rs, &seat_listener, s);
	if (layer_shell) {
		s->ls = river_layer_shell_v1_get_seat(layer_shell, rs);
		river_layer_shell_seat_v1_add_listener(s->ls, &ls_listener, s);
	}
	for (size_t i = 0; i < NKEYS; i++) {
		s->binds[i] = river_xkb_bindings_v1_get_xkb_binding(xkb, rs, keys[i].sym, keys[i].mods);
		river_xkb_binding_v1_add_listener(s->binds[i], &bind_listener, (void *)&keys[i]);
	}
	const uint32_t buttons[2] = { BTN_LEFT, BTN_RIGHT };
	for (int i = 0; i < 2; i++) {
		s->pbinds[i] = river_seat_v1_get_pointer_binding(rs, buttons[i], SUPER);
		river_pointer_binding_v1_add_listener(s->pbinds[i], &pbind_listener,
		                                      (void *)(intptr_t)(i == 0 ? OP_MOVE : OP_RESIZE));
	}
	s->binds_new = true;                      /* enable dopiero w manage */
	wl_list_insert(&seats, &s->link);
}

static void wm_session_locked(void *d, struct river_window_manager_v1 *m) {}
static void wm_session_unlocked(void *d, struct river_window_manager_v1 *m) {}

static const struct river_window_manager_v1_listener wm_listener = {
	.unavailable = wm_unavailable,
	.finished = wm_finished,
	.manage_start = wm_manage_start,
	.render_start = wm_render_start,
	.session_locked = wm_session_locked,
	.session_unlocked = wm_session_unlocked,
	.window = wm_window,
	.output = wm_output,
	.seat = wm_seat,
};

/* ---------- rejestr i main ---------- */

static void wo_geometry(void *d, struct wl_output *o, int32_t x, int32_t y, int32_t pw, int32_t ph,
                        int32_t sub, const char *make, const char *model, int32_t tr) {}
static void wo_mode(void *d, struct wl_output *o, uint32_t f, int32_t w, int32_t h, int32_t r) {}
static void wo_done(void *d, struct wl_output *o) {}
static void wo_scale(void *d, struct wl_output *o, int32_t f) {}

static void wo_name(void *data, struct wl_output *o, const char *name)
{
	struct wlout *wo = data;
	free(wo->conn);
	wo->conn = strdup(name);
}

static void wo_description(void *data, struct wl_output *o, const char *desc)
{
	struct wlout *wo = data;
	free(wo->desc);
	wo->desc = strdup(desc);
	if (restore_state == RESTORE_PENDING && wm)
		river_window_manager_v1_manage_dirty(wm);   /* może już da się odtworzyć układ */
}

static const struct wl_output_listener wo_listener = {
	.geometry = wo_geometry,
	.mode = wo_mode,
	.done = wo_done,
	.scale = wo_scale,
	.name = wo_name,
	.description = wo_description,
};

/* Najpierw tylko zapamiętujemy globale; bindujemy po roundtripie, żeby
 * żaden event wm nie przyszedł, zanim podepniemy listener. */
static uint32_t wm_name, wm_ver, xkb_name, xkb_ver, ls_name;
static bool seat_bound;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, river_window_manager_v1_interface.name)) {
		wm_name = name;
		wm_ver = ver < 6 ? ver : 6;
	} else if (!strcmp(iface, river_xkb_bindings_v1_interface.name)) {
		xkb_name = name;
		xkb_ver = ver < 3 ? ver : 3;
	} else if (!strcmp(iface, river_layer_shell_v1_interface.name)) {
		ls_name = name;
	} else if (!strcmp(iface, wl_compositor_interface.name)) {
		compositor = wl_registry_bind(reg, name, &wl_compositor_interface, ver < 4 ? ver : 4);
	} else if (!strcmp(iface, wp_viewporter_interface.name)) {
		viewporter = wl_registry_bind(reg, name, &wp_viewporter_interface, 1);
	} else if (!strcmp(iface, wp_fractional_scale_manager_v1_interface.name)) {
		fscale_mgr = wl_registry_bind(reg, name, &wp_fractional_scale_manager_v1_interface, 1);
	} else if (!strcmp(iface, wl_shm_interface.name)) {
		shm = wl_registry_bind(reg, name, &wl_shm_interface, 1);
	} else if (!strcmp(iface, wl_output_interface.name) && ver >= 4) {
		struct wlout *wo = calloc(1, sizeof *wo);
		wo->name = name;
		wo->wo = wl_registry_bind(reg, name, &wl_output_interface, 4);   /* v4: name, description */
		wl_output_add_listener(wo->wo, &wo_listener, wo);
		wl_list_insert(&wlouts, &wo->link);
	} else if (!strcmp(iface, wl_seat_interface.name) && !seat_bound) {
		seat_bound = true;
		urd_bind_seat(reg, name);
	}
}

static void reg_global_remove(void *d, struct wl_registry *reg, uint32_t name)
{
	struct wlout *wo, *tmp;
	wl_list_for_each_safe(wo, tmp, &wlouts, link) {
		if (wo->name != name)
			continue;
		wl_output_release(wo->wo);
		free(wo->desc);
		free(wo->conn);
		wl_list_remove(&wo->link);
		free(wo);
	}
}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

int main(void)
{
	signal(SIGCHLD, SIG_IGN);   /* dzieci (terminal itp.) nie zostają zombie */
	signal(SIGPIPE, SIG_IGN);   /* ściąga: fuzzel zamknięty przed zapisem nie może zabić WM */
	setlocale(LC_CTYPE, "");    /* UTF-8 w tytułach okien */

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "norns: brak połączenia z Waylandem\n");
		return 1;
	}

	wl_list_init(&windows);
	wl_list_init(&outputs);
	wl_list_init(&seats);
	wl_list_init(&wlouts);

	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);

	if (!wm_name || !xkb_name || !compositor || !shm) {
		fprintf(stderr, "norns: brak river_window_manager_v1 / river_xkb_bindings_v1 (to nie river?)\n");
		return 1;
	}
	load_theme();
	if (!urd_init())
		return 1;
	sfx_init();
	fx_init();
	layout_load();
	run_theme_script();         /* otwarte foote dostają paletę zapamiętanego motywu */

	/* xkb i layer shell przed wm: eventy output/seat od razu ich używają */
	xkb = wl_registry_bind(reg, xkb_name, &river_xkb_bindings_v1_interface, xkb_ver);
	if (ls_name)    /* opcjonalne: bez tego river nie pokaże paneli/launcherów */
		layer_shell = wl_registry_bind(reg, ls_name, &river_layer_shell_v1_interface, 1);
	wm = wl_registry_bind(reg, wm_name, &river_window_manager_v1_interface, wm_ver);
	river_window_manager_v1_add_listener(wm, &wm_listener, NULL);

	/* Pętla: Wayland + budzik co 2 s (zegar i sensory na pasku). */
	sensors_update();
	int tfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	struct itimerspec its = { .it_interval = { 2, 0 }, .it_value = { 2, 0 } };
	timerfd_settime(tfd, 0, &its, NULL);
	/* klatki animacji okien (jednorazowo, ~60 Hz) i żywej tapety */
	int afd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	int sfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);
	int gfd = timerfd_create(CLOCK_MONOTONIC, TFD_CLOEXEC);   /* wygaszacz (fx.c) */
	bool anim_armed = false;
	int sky_ms = -1, saver_ms = 0;
	int bfd = tray_init();          /* zasobnik po D-Bus; -1 = bez zasobnika */
	struct pollfd fds[6] = {
		{ .fd = wl_display_get_fd(display), .events = POLLIN },
		{ .fd = tfd, .events = POLLIN },
		{ .fd = afd, .events = POLLIN },
		{ .fd = sfd, .events = POLLIN },
		{ .fd = bfd, .events = POLLIN },  /* poll pomija ujemny fd */
		{ .fd = gfd, .events = POLLIN },
	};
	tray_dispatch();
	while (running) {
		while (wl_display_prepare_read(display) != 0)
			wl_display_dispatch_pending(display);
		wl_display_flush(display);
		if (poll(fds, 6, -1) < 0) {
			wl_display_cancel_read(display);
			continue;
		}
		if (fds[0].revents & POLLIN) {
			if (wl_display_read_events(display) < 0)
				break;
		} else {
			wl_display_cancel_read(display);
		}
		if (fds[0].revents & (POLLERR | POLLHUP))
			break;
		if (wl_display_dispatch_pending(display) < 0)
			break;

		if (fds[1].revents & POLLIN) {
			uint64_t n;
			if (read(tfd, &n, sizeof n) < 0)
				continue;
			sensors_update();
			urd_redraw_bars();
			notify_tick();
			if (restore_state == RESTORE_PENDING && now_ms() > restore_deadline)
				river_window_manager_v1_manage_dirty(wm);   /* opisy monitorów nie przyszły */
		}

		if (fds[4].revents & POLLIN)
			tray_dispatch();

		uint64_t n;
		if ((fds[2].revents & POLLIN) && read(afd, &n, sizeof n) > 0) {
			anim_armed = false;
			river_window_manager_v1_manage_dirty(wm);   /* kolejna klatka animacji */
		}
		if (anim_want && !anim_armed) {
			struct itimerspec one = { .it_value = { 0, 16 * 1000000 } };
			timerfd_settime(afd, 0, &one, NULL);
			anim_armed = true;
			anim_want = false;
		}

		bool sky_tick = (fds[3].revents & POLLIN) && read(sfd, &n, sizeof n) > 0;
		if (sky_tick || sky_ms < 0) {
			int want = urd_sky_frame(now_ms());
			if (want != sky_ms) {       /* zmiana tempa (kometa) albo wł./wył. */
				struct itimerspec it = { 0 };
				if (want > 0)
					it.it_value = it.it_interval = (struct timespec){ 0, want * 1000000L };
				timerfd_settime(sfd, 0, &it, NULL);
				sky_ms = want;
			}
		}

		/* wygaszacz: budzik jednorazowy, za tyle ms, ile chce następna klatka */
		bool saver_tick = (fds[5].revents & POLLIN) && read(gfd, &n, sizeof n) > 0;
		if (saver_tick || saver_on != (saver_ms > 0)) {
			saver_ms = fx_saver_frame(now_ms());
			struct itimerspec it = { .it_value = { saver_ms / 1000, saver_ms % 1000 * 1000000L } };
			timerfd_settime(gfd, 0, &it, NULL);
		}
	}

	/* zerwane połączenie: powiedz dlaczego, zamiast znikać po cichu */
	int err = wl_display_get_error(display);
	if (err) {
		const struct wl_interface *iface;
		uint32_t id;
		uint32_t code = wl_display_get_protocol_error(display, &iface, &id);
		if (iface)
			fprintf(stderr, "norns: błąd protokołu %s#%u, kod %u\n", iface->name, id, code);
		else
			fprintf(stderr, "norns: błąd połączenia z Waylandem (errno %d)\n", err);
		return 1;
	}

	river_window_manager_v1_destroy(wm);
	wl_display_disconnect(display);
	return 0;
}
