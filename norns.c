// norns — BSP window manager for river (river-window-management-v1)
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

/* ---------- config ---------- */

#define GAP    8
#define BORDER 2
#define NTAGS  9
#define C8(x)  ((uint32_t)(x) * 0x01010101u)   /* 8-bit -> 32-bit kolor */

static const uint32_t col_focus[4]   = { C8(0x8e), C8(0xc0), C8(0x7c), C8(0xff) }; /* gruvbox aqua */
static const uint32_t col_unfocus[4] = { C8(0x3c), C8(0x38), C8(0x36), C8(0xff) }; /* gruvbox bg1 */

static const char *const term[] = { "konsole", NULL };

enum action { SPAWN, CLOSE, QUIT, VIEW, SEND_TAG, FOCUS_OUT, SEND_OUT };

struct key {
	uint32_t mods;
	uint32_t sym;
	enum action act;
	int arg;                        /* numer tagu / kierunek ±1 */
	const char *const *cmd;
};

#define SUPER RIVER_SEAT_V1_MODIFIERS_MOD4
#define CTRL  RIVER_SEAT_V1_MODIFIERS_CTRL
#define TAGKEYS(n) \
	{ SUPER,        XKB_KEY_##n, VIEW,     (n) - 1, NULL }, \
	{ SUPER | CTRL, XKB_KEY_##n, SEND_TAG, (n) - 1, NULL }

static const struct key keys[] = {
	{ SUPER,        XKB_KEY_Return, SPAWN,     0, term },
	{ SUPER,        XKB_KEY_q,      CLOSE,     0, NULL },
	{ SUPER,        XKB_KEY_Escape, QUIT,      0, NULL },  /* wyjście z norns, nie z rivera */
	{ SUPER,        XKB_KEY_comma,  FOCUS_OUT, -1, NULL },
	{ SUPER,        XKB_KEY_period, FOCUS_OUT, +1, NULL },
	{ SUPER | CTRL, XKB_KEY_comma,  SEND_OUT,  -1, NULL },
	{ SUPER | CTRL, XKB_KEY_period, SEND_OUT,  +1, NULL },
	TAGKEYS(1), TAGKEYS(2), TAGKEYS(3), TAGKEYS(4), TAGKEYS(5),
	TAGKEYS(6), TAGKEYS(7), TAGKEYS(8), TAGKEYS(9),
};
#define NKEYS (sizeof keys / sizeof keys[0])

/* ---------- stan ---------- */

struct node {                       /* węzeł drzewa BSP */
	struct node *parent, *a, *b;    /* a/b tylko w węzłach wewnętrznych */
	struct win *win;                /* tylko w liściach */
	bool side_by_side;              /* podział pionową kreską (a | b) */
	int x, y, w, h;                 /* kafel z ostatniego layoutu */
};

struct output {
	struct wl_list link;
	struct river_output_v1 *ro;
	int x, y, w, h;
	struct river_layer_shell_output_v1 *lo;
	int ux, uy, uw, uh;             /* obszar bez paneli (ironbar itp.) */
	struct node *roots[NTAGS];      /* osobne drzewo na każdy tag */
	int tag;                        /* aktualnie widoczny tag */
};

struct win {
	struct wl_list link;
	struct river_window_v1 *rw;
	struct river_node_v1 *rn;
	struct output *out;             /* NULL = jeszcze nigdzie nie umieszczone */
	int tag;
	struct node *leaf;
	bool fresh;                     /* nowe okno, jeszcze bez ssd/tiled */
	int x, y, w, h;                 /* treść okna (bez ramki) */
};

struct seat {
	struct wl_list link;
	struct river_seat_v1 *rs;
	struct river_layer_shell_seat_v1 *ls;
	struct river_xkb_binding_v1 *binds[NKEYS];
	bool binds_new;
};

static struct wl_display *display;
static struct river_window_manager_v1 *wm;
static struct river_xkb_bindings_v1 *xkb;
static struct river_layer_shell_v1 *layer_shell;
static struct wl_list windows, outputs, seats;
static struct win *focused;
static bool focus_dirty, close_focused, running = true;
static int ptr_x, ptr_y;
static bool ptr_known, warp;

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
	leaf->parent = t;
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
		int in = GAP / 2 + BORDER;
		n->win->x = x + in;
		n->win->y = y + in;
		n->win->w = w - 2 * in > 1 ? w - 2 * in : 1;
		n->win->h = h - 2 * in > 1 ? h - 2 * in : 1;
	} else if (n->side_by_side) {
		layout(n->a, x, y, w / 2, h);
		layout(n->b, x + w / 2, y, w - w / 2, h);
	} else {
		layout(n->a, x, y, w, h / 2);
		layout(n->b, x, y + h / 2, w, h - h / 2);
	}
}

/* ---------- okna, monitory, fokus ---------- */

static void set_focus(struct win *w)
{
	if (focused != w) {
		focused = w;
		focus_dirty = true;
	}
}

static bool visible(struct win *w)
{
	return w->out && w->tag == w->out->tag;
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

static struct output *neighbor(struct output *o, int dir)
{
	struct wl_list *l = dir > 0 ? o->link.next : o->link.prev;
	if (l == &outputs)              /* przeskok przez głowę listy */
		l = dir > 0 ? l->next : l->prev;
	struct output *n;
	return wl_container_of(l, n, link);
}

static void detach(struct win *w)
{
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
	bsp_insert(&o->roots[tag], w, near);
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

/* Wołane z eventu pressed — zmieniamy tylko nasze dane, requesty idą w manage. */
static void run(const struct key *k)
{
	struct output *o = active_output(), *n;

	switch (k->act) {
	case SPAWN:
		spawn(k->cmd);
		break;
	case CLOSE:
		close_focused = true;
		break;
	case QUIT:
		river_window_manager_v1_stop(wm);
		break;
	case VIEW:
		if (o && o->tag != k->arg) {
			o->tag = k->arg;
			set_focus(first_on(o));
		}
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
	}
}

/* ---------- river_window_v1 ---------- */

static void win_closed(void *data, struct river_window_v1 *rw)
{
	struct win *w = data;
	if (w->leaf) {
		struct node *p = bsp_remove(&w->out->roots[w->tag], w);
		if (focused == w)
			set_focus(p ? first_leaf(p)->win : NULL);
	} else if (focused == w) {
		set_focus(NULL);
	}
	river_node_v1_destroy(w->rn);
	river_window_v1_destroy(rw);
	wl_list_remove(&w->link);
	free(w);
}

static void win_dimensions_hint(void *d, struct river_window_v1 *rw, int32_t a, int32_t b, int32_t c, int32_t e) {}
static void win_dimensions(void *d, struct river_window_v1 *rw, int32_t w, int32_t h) {}
static void win_app_id(void *d, struct river_window_v1 *rw, const char *s) {}
static void win_title(void *d, struct river_window_v1 *rw, const char *s) {}
static void win_parent(void *d, struct river_window_v1 *rw, struct river_window_v1 *p) {}
static void win_decoration_hint(void *d, struct river_window_v1 *rw, uint32_t h) {}
static void win_pointer_move_requested(void *d, struct river_window_v1 *rw, struct river_seat_v1 *s) {}
static void win_pointer_resize_requested(void *d, struct river_window_v1 *rw, struct river_seat_v1 *s, uint32_t e) {}
static void win_show_window_menu_requested(void *d, struct river_window_v1 *rw, int32_t x, int32_t y) {}
static void win_maximize_requested(void *d, struct river_window_v1 *rw) {}
static void win_unmaximize_requested(void *d, struct river_window_v1 *rw) {}
static void win_fullscreen_requested(void *d, struct river_window_v1 *rw, struct river_output_v1 *o) {}
static void win_exit_fullscreen_requested(void *d, struct river_window_v1 *rw) {}
static void win_minimize_requested(void *d, struct river_window_v1 *rw) {}
static void win_unreliable_pid(void *d, struct river_window_v1 *rw, int32_t pid) {}
static void win_presentation_hint(void *d, struct river_window_v1 *rw, uint32_t h) {}
static void win_identifier(void *d, struct river_window_v1 *rw, const char *s) {}
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

static void out_wl_output(void *d, struct river_output_v1 *ro, uint32_t name) {}
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

/* ---------- river_seat_v1 ---------- */

static void seat_removed(void *data, struct river_seat_v1 *rs)
{
	struct seat *s = data;
	for (size_t i = 0; i < NKEYS; i++)
		river_xkb_binding_v1_destroy(s->binds[i]);
	if (s->ls)
		river_layer_shell_seat_v1_destroy(s->ls);
	river_seat_v1_destroy(rs);
	wl_list_remove(&s->link);
	free(s);
}

/* focus-follows-mouse: najechanie = fokus */
static void seat_pointer_enter(void *d, struct river_seat_v1 *rs, struct river_window_v1 *rw)
{
	set_focus(river_window_v1_get_user_data(rw));
}

static void seat_window_interaction(void *d, struct river_seat_v1 *rs, struct river_window_v1 *rw)
{
	set_focus(river_window_v1_get_user_data(rw));
}

static void seat_pointer_position(void *d, struct river_seat_v1 *rs, int32_t x, int32_t y)
{
	ptr_x = x;
	ptr_y = y;
	ptr_known = true;
}

static void seat_wl_seat(void *d, struct river_seat_v1 *rs, uint32_t name) {}
static void seat_pointer_leave(void *d, struct river_seat_v1 *rs) {}
static void seat_shell_surface_interaction(void *d, struct river_seat_v1 *rs, struct river_shell_surface_v1 *ss) {}
static void seat_op_delta(void *d, struct river_seat_v1 *rs, int32_t dx, int32_t dy) {}
static void seat_op_release(void *d, struct river_seat_v1 *rs) {}
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

static void wm_manage_start(void *d, struct river_window_manager_v1 *m)
{
	struct output *o, *act = active_output();
	struct win *w;
	struct seat *s;

	/* nowe i osierocone okna lądują na aktywnym monitorze */
	wl_list_for_each(w, &windows, link) {
		if (w->out || !act)
			continue;
		attach(w, act, act->tag);
		if (w->fresh) {
			w->fresh = false;
			river_window_v1_use_ssd(w->rw);
			river_window_v1_set_tiled(w->rw, RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
			                                 RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);
			set_focus(w);
		}
	}

	if (close_focused && focused)
		river_window_v1_close(focused->rw);
	close_focused = false;

	/* fokus na niewidocznym oknie (np. wysłanym na inny tag) -> coś widocznego */
	if (act && (!focused || !visible(focused)))
		set_focus(first_on(act));

	wl_list_for_each(o, &outputs, link) {
		struct node *r = o->roots[o->tag];
		if (!r)
			continue;
		if (o->uw > 0 && o->uh > 0)
			layout(r, o->ux + GAP / 2, o->uy + GAP / 2, o->uw - GAP, o->uh - GAP);
		else
			layout(r, o->x + GAP / 2, o->y + GAP / 2, o->w - GAP, o->h - GAP);
	}
	wl_list_for_each(w, &windows, link)
		if (visible(w))
			river_window_v1_propose_dimensions(w->rw, w->w, w->h);

	wl_list_for_each(s, &seats, link) {
		if (s->binds_new) {
			for (size_t i = 0; i < NKEYS; i++)
				river_xkb_binding_v1_enable(s->binds[i]);
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

	river_window_manager_v1_manage_finish(wm);
}

static void wm_render_start(void *d, struct river_window_manager_v1 *m)
{
	struct win *w;
	wl_list_for_each(w, &windows, link) {
		if (!visible(w)) {
			river_window_v1_hide(w->rw);
			continue;
		}
		const uint32_t *c = w == focused ? col_focus : col_unfocus;
		river_window_v1_show(w->rw);
		river_node_v1_set_position(w->rn, w->x, w->y);
		river_window_v1_set_borders(w->rw, RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		                                   RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT,
		                            BORDER, c[0], c[1], c[2], c[3]);
	}
	river_window_manager_v1_render_finish(wm);
}

static void wm_window(void *d, struct river_window_manager_v1 *m, struct river_window_v1 *rw)
{
	struct win *w = calloc(1, sizeof *w);
	w->rw = rw;
	w->rn = river_window_v1_get_node(rw);
	w->fresh = true;
	river_window_v1_add_listener(rw, &win_listener, w);
	wl_list_insert(windows.prev, &w->link);   /* na koniec listy */
}

static void wm_output(void *d, struct river_window_manager_v1 *m, struct river_output_v1 *ro)
{
	struct output *o = calloc(1, sizeof *o);
	o->ro = ro;
	river_output_v1_add_listener(ro, &out_listener, o);
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

/* Najpierw tylko zapamiętujemy globale; bindujemy po roundtripie, żeby
 * żaden event wm nie przyszedł, zanim podepniemy listener. */
static uint32_t wm_name, wm_ver, xkb_name, xkb_ver, ls_name;

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
	}
}

static void reg_global_remove(void *d, struct wl_registry *reg, uint32_t name) {}

static const struct wl_registry_listener reg_listener = {
	.global = reg_global,
	.global_remove = reg_global_remove,
};

int main(void)
{
	signal(SIGCHLD, SIG_IGN);   /* dzieci (terminal itp.) nie zostają zombie */

	display = wl_display_connect(NULL);
	if (!display) {
		fprintf(stderr, "norns: brak połączenia z Waylandem\n");
		return 1;
	}

	wl_list_init(&windows);
	wl_list_init(&outputs);
	wl_list_init(&seats);

	struct wl_registry *reg = wl_display_get_registry(display);
	wl_registry_add_listener(reg, &reg_listener, NULL);
	wl_display_roundtrip(display);

	if (!wm_name || !xkb_name) {
		fprintf(stderr, "norns: brak river_window_manager_v1 / river_xkb_bindings_v1 (to nie river?)\n");
		return 1;
	}

	/* xkb i layer shell przed wm: eventy output/seat od razu ich używają */
	xkb = wl_registry_bind(reg, xkb_name, &river_xkb_bindings_v1_interface, xkb_ver);
	if (ls_name)    /* opcjonalne: bez tego river nie pokaże paneli/launcherów */
		layer_shell = wl_registry_bind(reg, ls_name, &river_layer_shell_v1_interface, 1);
	wm = wl_registry_bind(reg, wm_name, &river_window_manager_v1_interface, wm_ver);
	river_window_manager_v1_add_listener(wm, &wm_listener, NULL);

	while (running && wl_display_dispatch(display) != -1)
		;

	river_window_manager_v1_destroy(wm);
	wl_display_disconnect(display);
	return 0;
}
