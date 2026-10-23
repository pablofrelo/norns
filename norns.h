// norns — wspólne struktury dla norns.c (WM) i urd.c (pasek, tapeta)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>
#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

#define NTAGS 9

/* ---------- motywy ---------- */

struct theme {
	const char *name;               /* argument dla ~/.config/norns/theme */
	const char *label;              /* napis na pasku */
	const char *font;               /* wzorzec fontconfig */
	uint32_t bg, line, dim, fg, accent, warn, alarm;   /* 0xRRGGBB */
	uint32_t focus, unfocus;        /* ramki okien */
	uint32_t wall, dot;             /* tapeta */
};

extern const struct theme themes[];
extern int theme_idx;
#define THEME (&themes[theme_idx])

/* ---------- drzewo, okna, monitory ---------- */

struct node {                       /* węzeł drzewa BSP */
	struct node *parent, *a, *b;    /* a/b tylko w węzłach wewnętrznych */
	struct win *win;                /* tylko w liściach */
	bool side_by_side;              /* podział pionową kreską (a | b) */
	int x, y, w, h;                 /* kafel z ostatniego layoutu */
};

struct surf {                       /* powierzchnia rysowana przez norns */
	struct wl_surface *wl;
	struct river_shell_surface_v1 *ss;
	struct river_node_v1 *node;
	int x, y, w, h;                 /* gdzie i w jakim rozmiarze ma być */
	int drawn_w, drawn_h;           /* rozmiar ostatnio narysowanego bufora */
	bool dirty;
};

struct output {
	struct wl_list link;
	struct river_output_v1 *ro;
	int x, y, w, h;
	struct river_layer_shell_output_v1 *lo;
	int ux, uy, uw, uh;             /* obszar bez paneli (layer shell) */
	struct node *roots[NTAGS];      /* osobne drzewo na każdy tag */
	int tag;                        /* aktualnie widoczny tag */
	struct surf bar, wall;
	int tag_x[NTAGS + 1];           /* granice klikalnych tagów na pasku */
	int theme_x;                    /* od tego x klik przełącza motyw */
};

struct win {
	struct wl_list link;
	struct river_window_v1 *rw;
	struct river_node_v1 *rn;
	struct output *out;             /* NULL = jeszcze nigdzie nie umieszczone */
	int tag;
	struct node *leaf;
	bool fresh;                     /* nowe okno, jeszcze bez ssd/tiled */
	char *title;
	int x, y, w, h;                 /* treść okna (bez ramki) */
};

extern struct river_window_manager_v1 *wm;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct wl_list windows, outputs;
extern struct win *focused;

/* norns.c — akcje wołane z paska */
void view_tag(struct output *o, int tag);
void toggle_theme(void);

/* urd.c */
bool urd_init(void);
void urd_bind_seat(struct wl_registry *reg, uint32_t name);
int urd_bar_height(void);
void urd_output_init(struct output *o);
void urd_output_fini(struct output *o);
void urd_render(struct output *o);   /* tylko w trakcie sekwencji render */
