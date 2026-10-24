// norns — wspólne struktury dla norns.c (WM) i urd.c (pasek, tapeta)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>
#include <wayland-client.h>
#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"

#define NTAGS 9
#define GAP    12
#define BORDER 2

/* ---------- bufory (shm.c) ---------- */

struct buf {
	struct wl_buffer *wb;
	pixman_image_t *img;
	void *data;
	size_t size;
	int w, h;
	bool keep;                      /* wielokrotny: po release wraca do puli */
	bool busy;                      /* trzymany przez compositor */
};

struct buf *buf_new(int w, int h, bool alpha, bool keep);
void buf_attach(struct buf *b, struct wl_surface *s);
void buf_drop(struct buf *b);

/* ---------- motywy ---------- */

struct theme {
	const char *name;               /* argument dla ~/.config/norns/theme */
	const char *label;              /* napis na pasku */
	const char *font;               /* wzorzec fontconfig */
	const char *const *term;        /* terminal odpalany w tym motywie */
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
	float ratio;                    /* ile miejsca dostaje a (0.1–0.9) */
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
	uint32_t wl_name;               /* globalny wl_output tego monitora */
	int x, y, w, h;
	struct river_layer_shell_output_v1 *lo;
	int ux, uy, uw, uh;             /* obszar bez paneli (layer shell) */
	struct node *roots[NTAGS];      /* osobne drzewo na każdy tag */
	int tag;                        /* aktualnie widoczny tag */
	struct surf bar, wall;
	struct sky *sky;                /* żywa tapeta (urd.c) */
	int slide_from, slide_dir;      /* animacja przełączania tagów */
	uint64_t slide_t0;
	int ax, ay, aw, ah;             /* obszar na okna (bez paneli i paska) */
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
	char *title, *app_id;
	struct win *parent;             /* dialog -> okno, do którego należy */
	int min_w, min_h, max_w, max_h; /* podpowiedź rozmiaru od okna */
	bool floating;                  /* pływa nad kafelkami, poza drzewem BSP */
	int float_pending;              /* 0 nic, 1 rozmiar wg okna, 2 zaproponuj 60% ekranu */
	bool tiled_dirty;               /* trzeba powiedzieć oknu, czy jest kafelkiem */
	int dw, dh;                     /* rzeczywisty rozmiar (event dimensions) */
	int pw, ph;                     /* ostatnio zaproponowany rozmiar */
	int x, y, w, h;                 /* treść okna (bez ramki) — cel z layoutu */
	bool placed;                    /* pływające przesunięte ręcznie — bez centrowania */
	bool dragging;                  /* łapane myszą: bez animacji dojazdu */
	int drag_dx, drag_dy;           /* kafel niesiony za kursorem */

	/* animacja (anim.c) */
	bool shown, ever_shown, clip_on;
	int bx, by;                     /* pozycja wyświetlona (bez przesunięcia ślizgu) */
	int tx, ty;                     /* poprzedni cel, żeby wykryć zmianę */
	int mfx, mfy;                   /* skąd jedzie */
	uint64_t move_t0, open_t0;

	/* poświata / cień (deco.c) */
	struct wl_surface *deco_wl[4];  /* paski: góra, dół, lewo, prawo */
	struct river_decoration_v1 *deco[4];
	int deco_kind, deco_r, deco_w, deco_h;
	uint32_t deco_color;
};

static inline bool win_visible(const struct win *w)
{
	return w->out && w->tag == w->out->tag;
}

/* ---------- sensory (sensors.c) ---------- */

struct sensors {
	int cpu;                        /* % */
	double ram_gb;                  /* zajęte */
	int temp;                       /* °C */
	int bat;                        /* % */
	char bat_state;                 /* + ładuje, - rozładowuje, = pełna */
	bool has_temp, has_bat;
	bool lan, wifi, wg;
};

extern struct sensors sens;
void sensors_update(void);

extern struct river_window_manager_v1 *wm;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct wl_list windows, outputs;
extern struct win *focused;

/* anim.c */
extern bool anim_on;
uint64_t now_ms(void);
void anim_tag_switch(struct output *o, int from, int to);
void anim_render_window(struct win *w, uint64_t now);   /* tylko w sekwencji render */
bool anim_busy(uint64_t now);

/* deco.c */
void deco_init(struct win *w);
void deco_fini(struct win *w);
void deco_render(struct win *w);                        /* tylko w sekwencji render */

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
void urd_redraw_bars(void);          /* poza sekwencjami: zegar i sensory */
int urd_sky_frame(uint64_t now);     /* klatka żywej tapety; zwraca ms do następnej lub -1 */
void urd_cursor(const char *name);   /* kształt kursora, gdy żaden klient nie ma wskaźnika */
