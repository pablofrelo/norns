// norns — wspólne struktury dla norns.c (WM) i urd.c (pasek, tapeta)
#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <pixman.h>
#include <wayland-client.h>
#include "river-layer-shell-v1-client-protocol.h"
#include "river-window-management-v1-client-protocol.h"
#include "river-xkb-bindings-v1-client-protocol.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "config.h"                 /* GAP, BORDER, czasy animacji */

#define NTAGS 9

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
	const char *const *run;         /* uruchamianie programów (Super+D) */
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
	struct wp_viewport *vp;         /* skalowanie ułamkowe: bufor w pikselach, */
	struct wp_fractional_scale_v1 *fs;  /* wyświetlany w rozmiarze logicznym */
	int scale120;                   /* skala * 120 (120 = 1,0; 150 = 1,25) */
	int drawn_scale;
	bool alpha;                     /* bufor z przezroczystością */
	int font_pct;                   /* font względem paska; 0 = 100% */
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
	int tray_x0, tray_slot;         /* zasobnik na pasku (piksele bufora) */
	struct fx *fx;                  /* warstwa efektów trybu Nostromo (nostromo.c) */
	struct fx *crt;                 /* stałe linie kineskopu w trybie Nostromo */
	struct surf note;               /* powiadomienia (urd.c rysuje, verdandi.c trzyma treść) */
	struct surf menu;               /* menu aplikacji z zasobnika */
	bool menu_shown;
	int menu_rowh, menu_pad;        /* wysokość wiersza i margines (piksele bufora) */
	bool note_shown;
	int note_y[8];                  /* granice powiadomień w pikselach bufora (do klików) */
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
	char ident[40];                 /* stały identyfikator od rivera (przeżywa restart norns) */
	struct win *parent;             /* dialog -> okno, do którego należy */
	int min_w, min_h, max_w, max_h; /* podpowiedź rozmiaru od okna */
	bool floating;                  /* pływa nad kafelkami, poza drzewem BSP */
	int float_pending;              /* 0 nic, 1 rozmiar wg okna, 2 zaproponuj 60% ekranu */
	bool tiled_dirty;               /* trzeba powiedzieć oknu, czy jest kafelkiem */
	int dw, dh;                     /* rzeczywisty rozmiar (event dimensions) */
	int pw, ph;                     /* ostatnio zaproponowany rozmiar */
	int x, y, w, h;                 /* treść okna (bez ramki) — cel z layoutu */
	bool placed;                    /* pływające przesunięte ręcznie — bez centrowania */
	bool fullscreen;                /* pełny ekran (kafel w drzewie zostaje) */
	int fs_want;                    /* 0 nic, 1 wejdź, 2 wyjdź — wykonywane w manage */
	struct output *fs_out;          /* monitor, o który prosiło okno */
	bool dragging;                  /* łapane myszą: bez animacji dojazdu */
	bool drop, hidden, hiding;      /* terminal wysuwany (Super+`): schowany / chowa się */
	int drop_from, drop_to;         /* wysuwanie: skąd i dokąd jedzie (y) */
	uint64_t drop_t0;
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
	return w->out && !w->hidden && w->tag == w->out->tag;
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
	int vol;                        /* %, -1 = brak informacji */
	bool muted;
};

extern struct sensors sens;
void sensors_update(void);
void sensors_update_vol(void);

extern struct river_window_manager_v1 *wm;
extern struct wl_compositor *compositor;
extern struct wl_shm *shm;
extern struct wp_viewporter *viewporter;
extern struct wp_fractional_scale_manager_v1 *fscale_mgr;
extern struct wl_list windows, outputs;
extern struct win *focused;

/* anim.c */
extern bool anim_on;
uint64_t now_ms(void);
void anim_tag_switch(struct output *o, int from, int to);
void anim_render_window(struct win *w, uint64_t now);   /* tylko w sekwencji render */
bool anim_busy(uint64_t now);

/* nostromo.c — tryb Nostromo (Super+Ctrl+A) */
extern bool nostromo_on;
void nostromo_init(void);
void nostromo_toggle(void);
void nostromo_tick(bool fullscreen_visible);            /* co 2 s z pętli */
void nostromo_render(uint64_t now);                     /* w sekwencji render, na końcu */
bool nostromo_busy(uint64_t now);
int nostromo_jitter(const struct win *w, uint64_t now);
void nostromo_output_fini(struct output *o);
void sfx_typing(const char *text, int ms);              /* stukot klawiszy do powiadomienia */
void sfx_drop(bool up);                                 /* serwo terminala wysuwanego */
const char *sfx_path(const char *name);                 /* ścieżka wygenerowanego dźwięku */

/* deco.c */
void deco_init(struct win *w);
void deco_fini(struct win *w);
void deco_render(struct win *w);                        /* tylko w sekwencji render */

/* norns.c — akcje wołane z paska */
void view_tag(struct output *o, int tag);
void toggle_theme(void);
void power_action(int i);            /* 0 wyloguj, 1 restart, 2 wyłącz (norns-ctl power) */
void expect_popup(int x, int y);     /* po prawym kliku w zasobnik: następne okno to menu */

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

/* ---------- zasobnik (tray.c) ---------- */

#define TRAY_MAX 16

struct sd_bus_slot;
struct sd_bus;
struct tray_item {
	char *service, *path;           /* gdzie mieszka StatusNotifierItem */
	char owner[256];                /* unikalna nazwa na magistrali (":1.42") */
	char *id, *title;
	pixman_image_t *icon;           /* premultiplied ARGB, rozmiar oryginalny */
	bool attention;                 /* Status = NeedsAttention */
	char *menu;                     /* ścieżka com.canonical.dbusmenu albo NULL */
	pixman_image_t *tinted;         /* ikona w kolorze motywu (urd.c), z: */
	pixman_image_t *tint_src;
	uint32_t tint_rgb;
	bool item_is_menu;              /* lewy klik też otwiera menu */
	struct sd_bus_slot *sig_slot, *get_slot;
};

extern struct tray_item *tray_items[TRAY_MAX];
extern int tray_n;
int tray_init(void);                 /* zwraca fd magistrali albo -1 */
void tray_dispatch(void);
void tray_click(int i, int button, int x, int y, struct output *o);   /* 1 lewy, 2 środkowy, 3 prawy */

/* menu aplikacji z zasobnika (dbusmenu), rysowane przez urd pod paskiem */
#define TMENU_MAX 128
struct tmenu_entry {
	int32_t id, parent;
	char label[128];
	bool enabled, visible, separator, submenu;
	int toggle;                     /* 0 brak, 1 checkbox, 2 radio */
	bool checked;
};
struct tmenu {
	bool open;
	struct output *out;
	int x;                          /* lewa krawędź (globalne logiczne px) */
	int parent;                     /* pokazywany poziom: 0 = główny */
	int hover;                      /* wiersz pod kursorem, -1 = żaden */
	int n;
	struct tmenu_entry e[TMENU_MAX];
	int rows[TMENU_MAX + 1];        /* widoczne wiersze: indeks w e[], -1 = „wstecz” */
	int nrows;
};
extern struct tmenu tmenu;
void tmenu_rows(void);               /* przelicz widoczne wiersze */
void tmenu_activate(int row);        /* klik w wiersz */
void tmenu_close(void);

/* ---------- powiadomienia: verdandi (verdandi.c) ---------- */

#define NOTE_MAX 4
struct note {
	uint32_t id;
	char app[64], summary[256], body[512];
	int urgency;                    /* 0 niska, 1 zwykła, 2 krytyczna */
	bool has_default;               /* klik wywołuje akcję "default" */
	uint64_t t0, expire;            /* expire 0 = do kliknięcia */
};
extern struct note notes[NOTE_MAX];
extern int notes_n;
void notify_init(struct sd_bus *bus);
void notify_tick(void);
bool notify_busy(uint64_t now);
void notify_click(int i);
int note_type_ms(size_t chars);
size_t note_chars(const struct note *n);   /* znaki (nie bajty) tytułu i treści */
struct output *focus_output(void);
void ctl_init(struct sd_bus *bus);     /* norns.c: polecenia z norns-ctl */   /* norns.c: monitor z fokusem (albo pod kursorem) */

/* ---------- skróty i reguły (definicje w config.h) ---------- */

enum action { SPAWN, CLOSE, QUIT, VIEW, SEND_TAG, FOCUS_OUT, SEND_OUT, THEME_TOGGLE, FLOAT_TOGGLE,
              ANIM_TOGGLE, FULLSCREEN_TOGGLE, VOLUME, FOCUS_DIR, SWAP_DIR, NOSTROMO_TOGGLE,
              HELP, DROP_TOGGLE, SHOT, POWER_MENU };
enum { LEFT, DOWN, UP, RIGHT };

struct key {
	uint32_t mods;
	uint32_t sym;
	enum action act;
	int arg;                        /* numer tagu / kierunek ±1 */
	const char *const *cmd;
};

struct rule {
	const char *app_id;
	const char *output;             /* fragment opisu monitora */
	int tag;                        /* 1–9, 0 = aktualnie widoczny */
};

#define NORNS_TYPES_DONE
#include "config.h"                 /* tablice: tylko gdy zdefiniowano NORNS_MAIN */
