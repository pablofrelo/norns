// norns — BSP window manager for river (river-window-management-v1)
#define _POSIX_C_SOURCE 200809L
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wayland-client.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

/* ---------- config ---------- */

#define GAP    8
#define BORDER 2
#define C8(x)  ((uint32_t)(x) * 0x01010101u)   /* 8-bit -> 32-bit kolor */

static const uint32_t col_focus[4]   = { C8(0x8e), C8(0xc0), C8(0x7c), C8(0xff) }; /* gruvbox aqua */
static const uint32_t col_unfocus[4] = { C8(0x3c), C8(0x38), C8(0x36), C8(0xff) }; /* gruvbox bg1 */

static const char *const term[] = { "konsole", NULL };

enum action { SPAWN, CLOSE, QUIT };

struct key {
	uint32_t mods;
	uint32_t sym;
	enum action act;
	const char *const *cmd;
};

#define SUPER RIVER_SEAT_V1_MODIFIERS_MOD4
static const struct key keys[] = {
	{ SUPER, XKB_KEY_Return, SPAWN, term },
	{ SUPER, XKB_KEY_q,      CLOSE, NULL },
	{ SUPER, XKB_KEY_Escape, QUIT,  NULL },  /* wyjście z norns, nie z rivera */
};
#define NKEYS (sizeof keys / sizeof keys[0])

/* ---------- stan ---------- */

struct node {                       /* węzeł drzewa BSP */
	struct node *parent, *a, *b;    /* a/b tylko w węzłach wewnętrznych */
	struct win *win;                /* tylko w liściach */
	bool side_by_side;              /* podział pionową kreską (a | b) */
	int x, y, w, h;                 /* kafel z ostatniego layoutu */
};

struct win {
	struct wl_list link;
	struct river_window_v1 *rw;
	struct river_node_v1 *rn;
	struct node *leaf;              /* NULL = nowe, jeszcze nie w drzewie */
	int x, y, w, h;                 /* treść okna (bez ramki) */
};

struct output {
	struct wl_list link;
	struct river_output_v1 *ro;
	int x, y, w, h;
};

struct seat {
	struct wl_list link;
	struct river_seat_v1 *rs;
	struct river_xkb_binding_v1 *binds[NKEYS];
	bool binds_new;
};

static struct wl_display *display;
static struct river_window_manager_v1 *wm;
static struct river_xkb_bindings_v1 *xkb;
static struct wl_list windows, outputs, seats;
static struct node *root;
static struct win *focused;
static bool focus_dirty, close_focused, running = true;

/* ---------- BSP ---------- */

static struct node *first_leaf(struct node *n)
{
	while (!n->win)
		n = n->a;
	return n;
}

/* Nowe okno dzieli kafel okna z fokusem na pół. */
static void bsp_insert(struct win *w)
{
	struct node *leaf = calloc(1, sizeof *leaf);
	leaf->win = w;
	w->leaf = leaf;
	if (!root) {
		root = leaf;
		return;
	}
	struct node *t = focused && focused->leaf ? focused->leaf : first_leaf(root);
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
static struct node *bsp_remove(struct win *w)
{
	struct node *leaf = w->leaf, *p = leaf->parent;
	free(leaf);
	w->leaf = NULL;
	if (!p) {
		root = NULL;
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

/* ---------- akcje ---------- */

static void spawn(const char *const *cmd)
{
	if (fork() == 0) {
		setsid();
		execvp(cmd[0], (char *const *)cmd);
		_exit(1);
	}
}

static void set_focus(struct win *w)
{
	if (focused != w) {
		focused = w;
		focus_dirty = true;
	}
}

/* ---------- river_window_v1 ---------- */

static void win_closed(void *data, struct river_window_v1 *rw)
{
	struct win *w = data;
	if (w->leaf) {
		struct node *p = bsp_remove(w);
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

static void out_removed(void *data, struct river_output_v1 *ro)
{
	struct output *o = data;
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

/* ---------- klawisze ---------- */

static void bind_pressed(void *data, struct river_xkb_binding_v1 *xb)
{
	const struct key *k = data;
	switch (k->act) {
	case SPAWN: spawn(k->cmd); break;
	case CLOSE: close_focused = true; break;   /* close tylko w manage */
	case QUIT:  river_window_manager_v1_stop(wm); break;
	}
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

static void seat_wl_seat(void *d, struct river_seat_v1 *rs, uint32_t name) {}
static void seat_pointer_leave(void *d, struct river_seat_v1 *rs) {}
static void seat_shell_surface_interaction(void *d, struct river_seat_v1 *rs, struct river_shell_surface_v1 *ss) {}
static void seat_op_delta(void *d, struct river_seat_v1 *rs, int32_t dx, int32_t dy) {}
static void seat_op_release(void *d, struct river_seat_v1 *rs) {}
static void seat_pointer_position(void *d, struct river_seat_v1 *rs, int32_t x, int32_t y) {}
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
	struct win *w;

	wl_list_for_each(w, &windows, link) {
		if (w->leaf)
			continue;
		bsp_insert(w);
		river_window_v1_use_ssd(w->rw);
		river_window_v1_set_tiled(w->rw, RIVER_WINDOW_V1_EDGES_TOP | RIVER_WINDOW_V1_EDGES_BOTTOM |
		                                 RIVER_WINDOW_V1_EDGES_LEFT | RIVER_WINDOW_V1_EDGES_RIGHT);
		set_focus(w);
	}

	if (close_focused && focused)
		river_window_v1_close(focused->rw);
	close_focused = false;

	if (root && !wl_list_empty(&outputs)) {
		struct output *o = wl_container_of(outputs.next, o, link);  /* na razie 1 monitor */
		layout(root, o->x + GAP / 2, o->y + GAP / 2, o->w - GAP, o->h - GAP);
		wl_list_for_each(w, &windows, link)
			river_window_v1_propose_dimensions(w->rw, w->w, w->h);
	}

	struct seat *s;
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
	}
	focus_dirty = false;

	river_window_manager_v1_manage_finish(wm);
}

static void wm_render_start(void *d, struct river_window_manager_v1 *m)
{
	struct win *w;
	wl_list_for_each(w, &windows, link) {
		if (!w->leaf)
			continue;
		const uint32_t *c = w == focused ? col_focus : col_unfocus;
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
	river_window_v1_add_listener(rw, &win_listener, w);
	wl_list_insert(windows.prev, &w->link);   /* na koniec listy */
}

static void wm_output(void *d, struct river_window_manager_v1 *m, struct river_output_v1 *ro)
{
	struct output *o = calloc(1, sizeof *o);
	o->ro = ro;
	river_output_v1_add_listener(ro, &out_listener, o);
	wl_list_insert(outputs.prev, &o->link);
}

static void wm_seat(void *d, struct river_window_manager_v1 *m, struct river_seat_v1 *rs)
{
	struct seat *s = calloc(1, sizeof *s);
	s->rs = rs;
	river_seat_v1_add_listener(rs, &seat_listener, s);
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
static uint32_t wm_name, wm_ver, xkb_name, xkb_ver;

static void reg_global(void *d, struct wl_registry *reg, uint32_t name, const char *iface, uint32_t ver)
{
	if (!strcmp(iface, river_window_manager_v1_interface.name)) {
		wm_name = name;
		wm_ver = ver < 6 ? ver : 6;
	} else if (!strcmp(iface, river_xkb_bindings_v1_interface.name)) {
		xkb_name = name;
		xkb_ver = ver < 3 ? ver : 3;
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

	/* xkb przed wm: event seat od razu tworzy skróty */
	xkb = wl_registry_bind(reg, xkb_name, &river_xkb_bindings_v1_interface, xkb_ver);
	wm = wl_registry_bind(reg, wm_name, &river_window_manager_v1_interface, wm_ver);
	river_window_manager_v1_add_listener(wm, &wm_listener, NULL);

	while (running && wl_display_dispatch(display) != -1)
		;

	river_window_manager_v1_destroy(wm);
	wl_display_disconnect(display);
	return 0;
}
