// norns — konfiguracja. Po zmianie: make && ./run.sh (test) albo make install.
// Plik wczytywany dwa razy przez norns.h: najpierw stałe (dla wszystkich
// plików), potem — tylko w norns.c, gdy typy są już znane — tablice.

#ifndef NORNS_CONFIG_CONSTS
#define NORNS_CONFIG_CONSTS

/* ---------- wygląd i ruch ---------- */

#define GAP       12                /* przerwa między oknami; poświata = połowa */
#define BORDER    2                 /* ramka okna */
#define FLOAT_R   18                /* poświata / cień okien pływających */
#define SLIDE_MS  140               /* ślizg przy zmianie tagu */
#define MOVE_MS   120               /* dojazd okna na nowe miejsce */
#define OPEN_MS   160               /* otwieranie okna (kineskop) */
#define DROP_W        1280          /* terminal wysuwany: stały rozmiar (logiczne px), */
#define DROP_H        700           /* na mniejszym ekranie najwyżej 90% obszaru */
#define DROP_APP_ID   "norns-drop"
#define DROP_MS       500           /* wysuwanie od dołu / chowanie */
#define TRAY_TINT     1             /* ikonki w zasobniku w kolorze motywu (0 = oryginalne) */
#define NOTE_W        640           /* szerokość powiadomienia (logiczne px) */
#define NOTE_FONT     150           /* font powiadomień: % fontu paska (Spleen 16 → 24) */
#define NOTE_MS       6000          /* jak długo wisi zwykłe powiadomienie */
#define NOTE_CHAR_MS  40            /* tempo „pisania” tekstu (ms na znak) */
#define NOTE_TYPE_MAX 3500          /* ale całość najwyżej tyle ms */

#endif /* NORNS_CONFIG_CONSTS */

#if defined(NORNS_MAIN) && defined(NORNS_TYPES_DONE) && !defined(NORNS_CONFIG_ARRAYS)
#define NORNS_CONFIG_ARRAYS
#include <xkbcommon/xkbcommon-keysyms.h>

/* ---------- motywy (Super+T albo klik na pasku) ---------- */

/* Font i paleta foota ustalane są przy starcie okna — stąd osobna komenda na motyw. */
static const char *const term_gruvbox[] = { "foot", NULL };
static const char *const term_muthur[] = {
	"foot", "-o", "main.font=Spleen:pixelsize=24", "-o", "main.initial-color-theme=light", NULL
};

/* Uruchamianie programów (Super+D): fuzzel w kolorach motywu. */
static const char *const run_gruvbox[] = {
	"fuzzel", "--font=JetBrainsMono Nerd Font:size=11", "--border-radius=0", "--border-width=2",
	"--background=282828f0", "--text-color=ebdbb2ff", "--match-color=fabd2fff",
	"--selection-color=504945ff", "--selection-text-color=ebdbb2ff", "--border-color=8ec07cff", NULL
};
static const char *const run_muthur[] = {
	"fuzzel", "--font=Spleen:pixelsize=16", "--border-radius=0", "--border-width=2",
	"--background=050806f0", "--text-color=8cffb0ff", "--match-color=d4ffe0ff",
	"--selection-color=163d22ff", "--selection-text-color=d4ffe0ff", "--border-color=8cffb0ff", NULL
};

const struct theme themes[] = {
	{
		.name = "gruvbox", .label = "GRUVBOX",
		.font = "JetBrainsMono Nerd Font:pixelsize=14",
		.term = term_gruvbox, .run = run_gruvbox,
		.bg = 0x282828, .line = 0x504945, .dim = 0x928374, .fg = 0xebdbb2,
		.accent = 0x8ec07c, .warn = 0xfabd2f, .alarm = 0xfb4934,
		.focus = 0x8ec07c, .unfocus = 0x3c3836,
		.wall = 0x1d2021, .dot = 0x32302f,
	},
	{
		.name = "muthur", .label = "MU/TH/UR",
		.font = "Spleen:pixelsize=16",  /* bitmapowe .otb, 8x16 */
		.term = term_muthur, .run = run_muthur,
		.bg = 0x050806, .line = 0x163d22, .dim = 0x4f9a68, .fg = 0x8cffb0,
		.accent = 0xd4ffe0, .warn = 0xffb000, .alarm = 0xff5a4a,
		.focus = 0x8cffb0, .unfocus = 0x163d22,
		.wall = 0x050806, .dot = 0x163d22,
	},
};

/* ---------- programy ---------- */

static const char *const browser[] = { "firefox", NULL };
static const char *const files[] = { "dolphin", NULL };
/* HADLEY: włącza, a gdy już działa — wyłącza (po nazwie procesu, nie po linii poleceń,
 * żeby pgrep nie złapał samego sh) */
static const char *const hadley[] = { "/bin/sh", "-c",
	"if pgrep -x hadley >/dev/null; then pkill -x hadley; else exec \"$HOME/.local/bin/hadley\"; fi", NULL };
static const char *const vol_up[] = { "wpctl", "set-volume", "-l", "1.0", "@DEFAULT_AUDIO_SINK@", "5%+", NULL };
static const char *const vol_down[] = { "wpctl", "set-volume", "@DEFAULT_AUDIO_SINK@", "5%-", NULL };
static const char *const vol_mute[] = { "wpctl", "set-mute", "@DEFAULT_AUDIO_SINK@", "toggle", NULL };

/* ---------- skróty ---------- */

#define SUPER RIVER_SEAT_V1_MODIFIERS_MOD4
#undef CTRL                         /* glibc ma makro CTRL(x) w ttydefaults.h */
#define CTRL  RIVER_SEAT_V1_MODIFIERS_CTRL
#define SHIFT RIVER_SEAT_V1_MODIFIERS_SHIFT
#define TAGKEYS(n) \
	{ SUPER,        XKB_KEY_##n, VIEW,     (n) - 1, NULL }, \
	{ SUPER | CTRL, XKB_KEY_##n, SEND_TAG, (n) - 1, NULL }

static const struct key keys[] = {
	{ SUPER,        XKB_KEY_Return, SPAWN,     0, NULL },  /* NULL = terminal motywu */
	{ SUPER,        XKB_KEY_grave,  DROP_TOGGLE, 0, NULL },  /* terminal wysuwany od dołu */
	{ 0,            XKB_KEY_Print,  SHOT,      0, NULL },  /* zrzut monitora pod kursorem */
	{ SHIFT,        XKB_KEY_Print,  SHOT,      1, NULL },  /* zrzut zaznaczonego obszaru */
	{ SUPER,        XKB_KEY_Print,  SHOT,      2, NULL },  /* zrzut okna z fokusem */
	{ SUPER,        XKB_KEY_slash,  HELP,      0, NULL },  /* ściąga ze skrótami */
	{ SUPER,        XKB_KEY_F1,     HELP,      0, NULL },
	{ SUPER,        XKB_KEY_d,      SPAWN,     1, NULL },  /* 1 = uruchamianie programów motywu */
	{ SUPER,        XKB_KEY_b,      SPAWN,     0, browser },
	{ SUPER,        XKB_KEY_e,      SPAWN,     0, files },
	{ SUPER | SHIFT, XKB_KEY_h,     SPAWN,     0, hadley },  /* h i H: zależnie od tego, */
	{ SUPER | SHIFT, XKB_KEY_H,     SPAWN,     0, hadley },  /* jak river poda Shift */
	{ SUPER,        XKB_KEY_q,      CLOSE,     0, NULL },
	{ SUPER,        XKB_KEY_Escape, QUIT,      0, NULL },  /* wyjście z norns, nie z rivera */
	{ SUPER,        XKB_KEY_t,      THEME_TOGGLE, 0, NULL },
	{ SUPER,        XKB_KEY_f,      FLOAT_TOGGLE, 0, NULL },
	{ SUPER,        XKB_KEY_a,      ANIM_TOGGLE,  0, NULL },  /* efekty wł./wył. */
	{ SUPER | CTRL, XKB_KEY_a,      NOSTROMO_TOGGLE, 0, NULL },  /* tryb Nostromo */
	{ SUPER | CTRL, XKB_KEY_f,      FULLSCREEN_TOGGLE, 0, NULL },
	{ SUPER,        XKB_KEY_h,      FOCUS_DIR, LEFT,  NULL },
	{ SUPER,        XKB_KEY_j,      FOCUS_DIR, DOWN,  NULL },
	{ SUPER,        XKB_KEY_k,      FOCUS_DIR, UP,    NULL },
	{ SUPER,        XKB_KEY_l,      FOCUS_DIR, RIGHT, NULL },
	{ SUPER | CTRL, XKB_KEY_h,      SWAP_DIR,  LEFT,  NULL },
	{ SUPER | CTRL, XKB_KEY_j,      SWAP_DIR,  DOWN,  NULL },
	{ SUPER | CTRL, XKB_KEY_k,      SWAP_DIR,  UP,    NULL },
	{ SUPER | CTRL, XKB_KEY_l,      SWAP_DIR,  RIGHT, NULL },
	{ 0, XKB_KEY_XF86AudioRaiseVolume, VOLUME, 0, vol_up },
	{ 0, XKB_KEY_XF86AudioLowerVolume, VOLUME, 0, vol_down },
	{ 0, XKB_KEY_XF86AudioMute,        VOLUME, 0, vol_mute },
	{ SUPER,        XKB_KEY_comma,  FOCUS_OUT, -1, NULL },
	{ SUPER,        XKB_KEY_period, FOCUS_OUT, +1, NULL },
	{ SUPER | CTRL, XKB_KEY_comma,  SEND_OUT,  -1, NULL },
	{ SUPER | CTRL, XKB_KEY_period, SEND_OUT,  +1, NULL },
	TAGKEYS(1), TAGKEYS(2), TAGKEYS(3), TAGKEYS(4), TAGKEYS(5),
	TAGKEYS(6), TAGKEYS(7), TAGKEYS(8), TAGKEYS(9),
};
#define NKEYS (sizeof keys / sizeof keys[0])

/* Ściąga (Super+/ albo Super+F1) — przy zmianie skrótów popraw też tutaj. */
static const char *const help_lines[] = {
	"Super+Return          terminal",
	"Super+`               terminal wysuwany od dołu (to samo chowa)",
	"Super+D               uruchom program",
	"Super+B / E           przeglądarka / pliki",
	"Super+Shift+H         HADLEY (kamery) wł./wył.",
	"Super+Q               zamknij okno",
	"Super+H J K L         fokus w lewo/dół/górę/prawo (też między monitorami)",
	"Super+Ctrl+H J K L    zamień okno z sąsiadem",
	"Super+1…9             pokaż tag (na monitorze z fokusem)",
	"Super+Ctrl+1…9        wyślij okno na tag",
	"Super+, / .           poprzedni / następny monitor",
	"Super+Ctrl+, / .      wyślij okno na poprzedni / następny monitor",
	"Super+F               okno pływające / kafel",
	"Super+Ctrl+F          pełny ekran",
	"Super+T               zmień motyw",
	"Super+A               animacje wł./wył.",
	"Super+Ctrl+A          tryb Nostromo",
	"Super+lewy przycisk   przesuń / zamień kafle",
	"Super+prawy przycisk  rozmiar / proporcje podziału",
	"Super+Escape          wyjdź z norns",
	"Print                 zrzut monitora (plik + schowek)",
	"Shift+Print           zrzut zaznaczonego obszaru",
	"Super+Print           zrzut okna z fokusem",
	"Super+/ albo F1       ta ściąga",
	NULL
};

/* ---------- reguły ---------- */

/* Gdzie lądują nowe okna aplikacji. Monitor po fragmencie opisu wl_output
 * (model + numer seryjny + złącze, zobacz: wlr-randr) — numer seryjny się
 * nie zmienia, nazwy DP-n potrafią. tag: 1–9, 0 = aktualnie widoczny.
 * Gdy monitora nie ma, okno ląduje normalnie, pod kursorem. */
static const struct rule rules[] = {
	{ "firefox", "1O0R9HA001785", 0 },     /* lewy AOC */
	{ "foot",    "1O0R9HA001780", 0 },     /* prawy AOC */
};
#define NRULES (sizeof rules / sizeof rules[0])

/* Okna, które zawsze pływają (np. wybór pliku z portalu, który nie podaje rodzica). */
static const char *const float_app_ids[] = {
	"xdg-desktop-portal-gtk",
	"org.freedesktop.impl.portal.desktop.kde",
	NULL,
};

#endif /* NORNS_CONFIG_ARRAYS */
