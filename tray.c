// tray — zasobnik systemowy (StatusNotifierItem) po D-Bus, przez sd-bus.
//
// Pod norns nikt inny nie zbiera ikonek, więc urd jest naraz:
//   watcher (org.kde.StatusNotifierWatcher) — rejestr, do którego aplikacje
//            zgłaszają swoje ikonki; jeśli działa już inny (np. waybar), używamy jego,
//   host    — pobiera ikonki i rysuje je na pasku, przekazuje kliknięcia.
// Ikona: gotowe piksele (IconPixmap, tak robi Electron) albo PNG po nazwie.
#define _GNU_SOURCE
#include <errno.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <systemd/sd-bus.h>
#include "norns.h"

#define WATCHER_NAME  "org.kde.StatusNotifierWatcher"
#define WATCHER_PATH  "/StatusNotifierWatcher"
#define ITEM_IFACE    "org.kde.StatusNotifierItem"
#define MENU_IFACE    "com.canonical.dbusmenu"

struct tray_item *tray_items[TRAY_MAX];
int tray_n;

static sd_bus *bus;
static bool own_watcher;            /* to my jesteśmy watcherem */
static char host_name[64];

/* ---------- ikony ---------- */

/* IconPixmap: ARGB32 big-endian, bez premultiplied — pixman chce premultiplied, native. */
static pixman_image_t *from_argb_be(int w, int h, const uint8_t *src)
{
	uint32_t *px = malloc((size_t)w * h * 4);
	if (!px)
		return NULL;
	for (int i = 0; i < w * h; i++) {
		uint32_t a = src[4 * i], r = src[4 * i + 1], g = src[4 * i + 2], b = src[4 * i + 3];
		px[i] = a << 24 | (r * a / 255) << 16 | (g * a / 255) << 8 | (b * a / 255);
	}
	return pixman_image_create_bits(PIXMAN_a8r8g8b8, w, h, px, w * 4);
}

static void icon_free(pixman_image_t *img)
{
	if (!img)
		return;
	void *data = pixman_image_get_data(img);
	pixman_image_unref(img);
	free(data);
}

static pixman_image_t *load_png(const char *path)
{
	png_image png = { .version = PNG_IMAGE_VERSION };
	if (!png_image_begin_read_from_file(&png, path))
		return NULL;
	png.format = PNG_FORMAT_ARGB;           /* bajty A,R,G,B — jak IconPixmap */
	uint8_t *buf = malloc(PNG_IMAGE_SIZE(png));
	if (!buf || !png_image_finish_read(&png, NULL, buf, 0, NULL)) {
		free(buf);
		png_image_free(&png);
		return NULL;
	}
	pixman_image_t *img = from_argb_be(png.width, png.height, buf);
	free(buf);
	return img;
}

/* PNG po nazwie: ścieżka z aplikacji, hicolor (od największych), pixmaps. */
static pixman_image_t *icon_by_name(const char *name, const char *theme_path)
{
	static const char *const sizes[] = { "64x64", "48x48", "32x32", "24x24", "22x22", "16x16", "128x128", "256x256" };
	static const char *const cats[] = { "apps", "status" };
	char path[512];
	pixman_image_t *img;

	if (name[0] == '/')
		return load_png(name);
	if (theme_path && *theme_path) {
		snprintf(path, sizeof path, "%s/%s.png", theme_path, name);
		if ((img = load_png(path)))
			return img;
		for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
			for (int c = 0; c < 2; c++) {
				snprintf(path, sizeof path, "%s/hicolor/%s/%s/%s.png", theme_path, sizes[s], cats[c], name);
				if ((img = load_png(path)))
					return img;
			}
	}
	const char *home = getenv("HOME");
	const char *roots[] = { "/usr/share/icons/hicolor", NULL, NULL };
	char local[256];
	if (home) {
		snprintf(local, sizeof local, "%s/.local/share/icons/hicolor", home);
		roots[1] = local;
	}
	for (int r = 0; r < 2 && roots[r]; r++)
		for (size_t s = 0; s < sizeof sizes / sizeof *sizes; s++)
			for (int c = 0; c < 2; c++) {
				snprintf(path, sizeof path, "%s/%s/%s/%s.png", roots[r], sizes[s], cats[c], name);
				if ((img = load_png(path)))
					return img;
			}
	snprintf(path, sizeof path, "/usr/share/pixmaps/%s.png", name);
	return load_png(path);
}

/* ---------- elementy ---------- */

static struct tray_item *find(const char *service, const char *path)
{
	for (int i = 0; i < tray_n; i++)
		if (!strcmp(tray_items[i]->service, service) && !strcmp(tray_items[i]->path, path))
			return tray_items[i];
	return NULL;
}

static void item_clear(struct tray_item *it)
{
	sd_bus_slot_unref(it->sig_slot);
	sd_bus_slot_unref(it->get_slot);
	icon_free(it->icon);
	icon_free(it->tinted);
	free(it->service);
	free(it->path);
	free(it->id);
	free(it->title);
	free(it->menu);
	free(it);
}

/* Odpowiedź na GetAll: wyciągamy to, czego potrzebujemy do narysowania. */
static int on_props(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	struct tray_item *it = userdata;
	sd_bus_slot_unref(it->get_slot);
	it->get_slot = NULL;
	if (sd_bus_message_is_method_error(m, NULL))
		return 0;

	char *icon_name = NULL, *theme_path = NULL;
	pixman_image_t *best = NULL;
	int best_d = 1 << 30, want = 32;

	if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
		return 0;
	while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
		const char *key;
		sd_bus_message_read(m, "s", &key);
		if (!strcmp(key, "IconPixmap")) {
			sd_bus_message_enter_container(m, 'v', "a(iiay)");
			sd_bus_message_enter_container(m, 'a', "(iiay)");
			while (sd_bus_message_enter_container(m, 'r', "iiay") > 0) {
				int w, h;
				const void *data;
				size_t len;
				sd_bus_message_read(m, "ii", &w, &h);
				sd_bus_message_read_array(m, 'y', &data, &len);
				int d = abs(w - want);
				if (w > 0 && h > 0 && len >= (size_t)w * h * 4 && d < best_d) {
					icon_free(best);
					best = from_argb_be(w, h, data);
					best_d = d;
				}
				sd_bus_message_exit_container(m);
			}
			sd_bus_message_exit_container(m);
			sd_bus_message_exit_container(m);
		} else if (!strcmp(key, "Menu")) {
			const char *v = NULL;
			if (sd_bus_message_read(m, "v", "o", &v) < 0)
				sd_bus_message_skip(m, "v");
			free(it->menu);
			it->menu = v && *v && strcmp(v, "/") ? strdup(v) : NULL;
		} else if (!strcmp(key, "ItemIsMenu")) {
			int b = 0;
			if (sd_bus_message_read(m, "v", "b", &b) < 0)
				sd_bus_message_skip(m, "v");
			it->item_is_menu = b;
		} else if (!strcmp(key, "IconName") || !strcmp(key, "IconThemePath") ||
		           !strcmp(key, "Status") || !strcmp(key, "Id") || !strcmp(key, "Title")) {
			const char *v = NULL;
			if (sd_bus_message_read(m, "v", "s", &v) < 0)
				sd_bus_message_skip(m, "v");
			else if (!strcmp(key, "IconName"))
				icon_name = v && *v ? strdup(v) : NULL;
			else if (!strcmp(key, "IconThemePath"))
				theme_path = v && *v ? strdup(v) : NULL;
			else if (!strcmp(key, "Status"))
				it->attention = v && !strcmp(v, "NeedsAttention");
			else if (!strcmp(key, "Id")) {
				free(it->id);
				it->id = v ? strdup(v) : NULL;
			} else {
				free(it->title);
				it->title = v ? strdup(v) : NULL;
			}
		} else {
			sd_bus_message_skip(m, "v");
		}
		sd_bus_message_exit_container(m);
	}

	/* nazwa z motywu ma pierwszeństwo nad pikselami, jak w innych zasobnikach */
	pixman_image_t *img = icon_name ? icon_by_name(icon_name, theme_path) : NULL;
	if (img) {
		icon_free(best);
		best = img;
	}
	if (best || !it->icon) {
		icon_free(it->icon);
		icon_free(it->tinted);              /* wersja w kolorze motywu powstanie od nowa */
		it->tinted = NULL;
		it->icon = best;
	}
	fprintf(stderr, "norns: zasobnik %s (%s) ikona=%s%s\n", it->id ? it->id : "?", it->service,
	        it->icon ? "jest" : "BRAK", icon_name && !img ? " (nazwa bez PNG — może SVG)" : "");
	free(icon_name);
	free(theme_path);
	urd_redraw_bars();
	return 0;
}

static void item_refresh(struct tray_item *it)
{
	sd_bus_slot_unref(it->get_slot);
	it->get_slot = NULL;
	sd_bus_call_method_async(bus, &it->get_slot, it->service, it->path,
	                         "org.freedesktop.DBus.Properties", "GetAll", on_props, it, "s", ITEM_IFACE);
}

/* Nowa ikona / status — pobieramy właściwości jeszcze raz. */
static int on_item_signal(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *sender = sd_bus_message_get_sender(m), *path = sd_bus_message_get_path(m);
	for (int i = 0; i < tray_n; i++) {
		struct tray_item *it = tray_items[i];
		/* service może być nazwą dobrze znaną — porównujemy ścieżkę i nadawcę-właściciela */
		if (!strcmp(it->path, path) && (!strcmp(it->owner, sender) || !strcmp(it->service, sender)))
			item_refresh(it);
	}
	return 0;
}

static void item_add(const char *service, const char *path)
{
	if (find(service, path) || tray_n >= TRAY_MAX)
		return;
	struct tray_item *it = calloc(1, sizeof *it);
	tray_items[tray_n++] = it;
	it->service = strdup(service);
	it->path = strdup(path);
	/* właściciel unikalny (":1.42") — po nim poznamy zniknięcie aplikacji i sygnały */
	const char *owner = service;
	sd_bus_creds *creds = NULL;
	if (service[0] != ':' && sd_bus_get_name_creds(bus, service, SD_BUS_CREDS_UNIQUE_NAME, &creds) >= 0)
		sd_bus_creds_get_unique_name(creds, &owner);
	snprintf(it->owner, sizeof it->owner, "%s", owner);
	sd_bus_creds_unref(creds);

	char match[512];
	snprintf(match, sizeof match, "type='signal',sender='%s',path='%s',interface='%s'", it->owner, path, ITEM_IFACE);
	sd_bus_add_match(bus, &it->sig_slot, match, on_item_signal, NULL);
	item_refresh(it);
}

static void item_remove_owner(const char *owner)
{
	for (int i = 0; i < tray_n;) {
		struct tray_item *it = tray_items[i];
		if (strcmp(it->owner, owner) && strcmp(it->service, owner)) {
			i++;
			continue;
		}
		char full[600];
		snprintf(full, sizeof full, "%s%s", it->service, it->path);
		item_clear(it);
		memmove(&tray_items[i], &tray_items[i + 1], (tray_n - i - 1) * sizeof *tray_items);
		tray_n--;
		if (own_watcher)
			sd_bus_emit_signal(bus, WATCHER_PATH, WATCHER_NAME, "StatusNotifierItemUnregistered", "s", full);
	}
	urd_redraw_bars();
}

/* "service" albo "service/ścieżka" albo sama "/ścieżka" (wtedy service = nadawca). */
static void item_add_str(const char *str, const char *sender)
{
	char service[256], path[256] = "/StatusNotifierItem";
	if (str[0] == '/') {
		snprintf(service, sizeof service, "%s", sender);
		snprintf(path, sizeof path, "%s", str);
	} else {
		const char *slash = strchr(str, '/');
		if (slash) {
			snprintf(service, sizeof service, "%.*s", (int)(slash - str), str);
			snprintf(path, sizeof path, "%s", slash);
		} else {
			snprintf(service, sizeof service, "%s", str);
		}
	}
	item_add(service, path);
}

/* ---------- watcher (gdy nikt inny go nie prowadzi) ---------- */

static int m_register_item(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *str;
	if (sd_bus_message_read(m, "s", &str) < 0)
		return -EINVAL;
	const char *sender = sd_bus_message_get_sender(m);
	item_add_str(str, sender);
	if (tray_n > 0) {
		struct tray_item *it = tray_items[tray_n - 1];
		char full[600];
		snprintf(full, sizeof full, "%s%s", it->service, it->path);
		sd_bus_emit_signal(bus, WATCHER_PATH, WATCHER_NAME, "StatusNotifierItemRegistered", "s", full);
	}
	return sd_bus_reply_method_return(m, "");
}

static int m_register_host(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	return sd_bus_reply_method_return(m, "");
}

static int p_items(sd_bus *b, const char *path, const char *iface, const char *prop,
                   sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
	sd_bus_message_open_container(reply, 'a', "s");
	for (int i = 0; i < tray_n; i++) {
		char full[600];
		snprintf(full, sizeof full, "%s%s", tray_items[i]->service, tray_items[i]->path);
		sd_bus_message_append(reply, "s", full);
	}
	return sd_bus_message_close_container(reply);
}

static int p_true(sd_bus *b, const char *path, const char *iface, const char *prop,
                  sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
	return sd_bus_message_append(reply, "b", 1);
}

static int p_version(sd_bus *b, const char *path, const char *iface, const char *prop,
                     sd_bus_message *reply, void *userdata, sd_bus_error *err)
{
	return sd_bus_message_append(reply, "i", 0);
}

static const sd_bus_vtable watcher_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("RegisterStatusNotifierItem", "s", "", m_register_item, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("RegisterStatusNotifierHost", "s", "", m_register_host, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_PROPERTY("RegisteredStatusNotifierItems", "as", p_items, 0, 0),
	SD_BUS_PROPERTY("IsStatusNotifierHostRegistered", "b", p_true, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_PROPERTY("ProtocolVersion", "i", p_version, 0, SD_BUS_VTABLE_PROPERTY_CONST),
	SD_BUS_SIGNAL("StatusNotifierItemRegistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierItemUnregistered", "s", 0),
	SD_BUS_SIGNAL("StatusNotifierHostRegistered", "", 0),
	SD_BUS_VTABLE_END,
};

/* Aplikacja zniknęła z magistrali — jej ikonka też. */
static int on_name_owner(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *name, *old, *new;
	if (sd_bus_message_read(m, "sss", &name, &old, &new) < 0)
		return 0;
	if (*old && !*new)
		item_remove_owner(name);
	return 0;
}

/* ---------- host przy cudzym watcherze ---------- */

static int on_ext_registered(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	const char *str;
	if (sd_bus_message_read(m, "s", &str) >= 0)
		item_add_str(str, sd_bus_message_get_sender(m));
	return 0;
}

static void use_external_watcher(void)
{
	sd_bus_match_signal(bus, NULL, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
	                    "StatusNotifierItemRegistered", on_ext_registered, NULL);
	sd_bus_call_method(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
	                   "RegisterStatusNotifierHost", NULL, NULL, "s", host_name);
	char **items = NULL;
	if (sd_bus_get_property_strv(bus, WATCHER_NAME, WATCHER_PATH, WATCHER_NAME,
	                             "RegisteredStatusNotifierItems", NULL, &items) >= 0 && items) {
		for (char **s = items; *s; s++) {
			item_add_str(*s, "");
			free(*s);
		}
		free(items);
	}
}

/* ---------- start, pętla, kliknięcia ---------- */

int tray_init(void)
{
	if (sd_bus_open_user(&bus) < 0) {
		fprintf(stderr, "norns: brak sesyjnej magistrali D-Bus — zasobnik wyłączony\n");
		bus = NULL;
		return -1;
	}
	notify_init(bus);                   /* powiadomienia na tej samej magistrali */
	ctl_init(bus);                      /* i polecenia z norns-ctl */
	snprintf(host_name, sizeof host_name, "org.kde.StatusNotifierHost-%d", getpid());
	sd_bus_request_name(bus, host_name, 0);
	sd_bus_match_signal(bus, NULL, "org.freedesktop.DBus", "/org/freedesktop/DBus",
	                    "org.freedesktop.DBus", "NameOwnerChanged", on_name_owner, NULL);

	if (sd_bus_add_object_vtable(bus, NULL, WATCHER_PATH, WATCHER_NAME, watcher_vtable, NULL) >= 0 &&
	    sd_bus_request_name(bus, WATCHER_NAME, 0) >= 0) {
		own_watcher = true;
		sd_bus_emit_signal(bus, WATCHER_PATH, WATCHER_NAME, "StatusNotifierHostRegistered", "");
		fprintf(stderr, "norns: zasobnik — jestem watcherem\n");
	} else {
		fprintf(stderr, "norns: zasobnik — działa inny watcher, korzystam z niego\n");
		use_external_watcher();
	}
	return sd_bus_get_fd(bus);
}

void tray_dispatch(void)
{
	if (!bus)
		return;
	while (sd_bus_process(bus, NULL) > 0)
		;
	sd_bus_flush(bus);
}

/* ---------- menu aplikacji (com.canonical.dbusmenu) ---------- */

struct tmenu tmenu = { .hover = -1 };
static char menu_service[256], menu_path[256];
static sd_bus_slot *menu_slot;
static struct { int i, x, y; } menu_fallback;   /* gdy menu się nie uda: ContextMenu */

/* Jeden węzeł układu (ia{sv}av) i rekurencyjnie jego dzieci. */
static void parse_node(sd_bus_message *m, int32_t parent, int depth)
{
	int32_t id;
	if (sd_bus_message_read(m, "i", &id) < 0)
		return;
	struct tmenu_entry e = { .id = id, .parent = parent, .enabled = true, .visible = true };
	if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			const char *key, *str;
			int32_t iv;
			int bv;
			sd_bus_message_read(m, "s", &key);
			if (!strcmp(key, "label") && sd_bus_message_read(m, "v", "s", &str) >= 0) {
				char *w = e.label;                          /* bez podkreśleń mnemonik */
				for (const char *r = str; *r && w < e.label + sizeof e.label - 1; r++) {
					if (*r == '_' && r[1] == '_')
						*w++ = *r++;
					else if (*r != '_')
						*w++ = *r;
				}
				*w = 0;
			} else if (!strcmp(key, "enabled") && sd_bus_message_read(m, "v", "b", &bv) >= 0) {
				e.enabled = bv;
			} else if (!strcmp(key, "visible") && sd_bus_message_read(m, "v", "b", &bv) >= 0) {
				e.visible = bv;
			} else if (!strcmp(key, "type") && sd_bus_message_read(m, "v", "s", &str) >= 0) {
				e.separator = !strcmp(str, "separator");
			} else if (!strcmp(key, "toggle-type") && sd_bus_message_read(m, "v", "s", &str) >= 0) {
				e.toggle = !strcmp(str, "checkmark") ? 1 : !strcmp(str, "radio") ? 2 : 0;
			} else if (!strcmp(key, "toggle-state") && sd_bus_message_read(m, "v", "i", &iv) >= 0) {
				e.checked = iv == 1;
			} else if (!strcmp(key, "children-display") && sd_bus_message_read(m, "v", "s", &str) >= 0) {
				e.submenu = !strcmp(str, "submenu");
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	if (depth > 0 && tmenu.n < TMENU_MAX)              /* korzeń (id 0) nie jest wierszem */
		tmenu.e[tmenu.n++] = e;
	if (sd_bus_message_enter_container(m, 'a', "v") >= 0) {
		bool any = false;
		while (sd_bus_message_enter_container(m, 'v', "(ia{sv}av)") > 0) {
			if (sd_bus_message_enter_container(m, 'r', "ia{sv}av") > 0) {
				parse_node(m, id, depth + 1);
				sd_bus_message_exit_container(m);
				any = true;
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
		if (any && depth > 0)                          /* ma dzieci = podmenu, nawet bez podpowiedzi */
			for (int i = 0; i < tmenu.n; i++)
				if (tmenu.e[i].id == id)
					tmenu.e[i].submenu = true;
	}
}

void tmenu_rows(void)
{
	tmenu.nrows = 0;
	if (tmenu.parent != 0)
		tmenu.rows[tmenu.nrows++] = -1;                    /* „‹ wstecz” */
	for (int i = 0; i < tmenu.n && tmenu.nrows < TMENU_MAX; i++)
		if (tmenu.e[i].parent == tmenu.parent && tmenu.e[i].visible)
			tmenu.rows[tmenu.nrows++] = i;
}

static void context_menu_fallback(void)
{
	int i = menu_fallback.i;
	if (i < 0 || i >= tray_n)
		return;
	expect_popup(menu_fallback.x, menu_fallback.y);
	sd_bus_call_method_async(bus, NULL, tray_items[i]->service, tray_items[i]->path, ITEM_IFACE,
	                         "ContextMenu", NULL, NULL, "ii", menu_fallback.x, menu_fallback.y);
}

static int on_layout(sd_bus_message *m, void *userdata, sd_bus_error *err)
{
	sd_bus_slot_unref(menu_slot);
	menu_slot = NULL;
	uint32_t rev;
	tmenu.n = 0;
	if (!sd_bus_message_is_method_error(m, NULL) && sd_bus_message_read(m, "u", &rev) >= 0 &&
	    sd_bus_message_enter_container(m, 'r', "ia{sv}av") > 0) {
		parse_node(m, 0, 0);
		sd_bus_message_exit_container(m);
	}
	if (tmenu.n == 0) {                                    /* nic nie przyszło — stara droga */
		fprintf(stderr, "norns: menu z zasobnika puste, próbuję ContextMenu\n");
		context_menu_fallback();
		sd_bus_flush(bus);
		return 0;
	}
	tmenu.parent = 0;
	tmenu.hover = -1;
	tmenu_rows();
	tmenu.open = true;
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		o->menu.dirty = true;
	river_window_manager_v1_manage_dirty(wm);
	return 0;
}

static void menu_open(struct tray_item *it, int x, struct output *o)
{
	snprintf(menu_service, sizeof menu_service, "%s", it->service);
	snprintf(menu_path, sizeof menu_path, "%s", it->menu);
	tmenu.out = o;
	tmenu.x = x;
	/* AboutToShow daje aplikacji szansę odświeżyć menu (nm-applet: lista sieci) */
	sd_bus_call_method_async(bus, NULL, menu_service, menu_path, MENU_IFACE, "AboutToShow", NULL, NULL, "i", 0);
	sd_bus_slot_unref(menu_slot);
	menu_slot = NULL;
	sd_bus_call_method_async(bus, &menu_slot, menu_service, menu_path, MENU_IFACE, "GetLayout",
	                         on_layout, NULL, "iias", 0, -1, 0);
}

void tmenu_close(void)
{
	if (!tmenu.open)
		return;
	tmenu.open = false;
	river_window_manager_v1_manage_dirty(wm);
}

void tmenu_activate(int row)
{
	if (!tmenu.open || row < 0 || row >= tmenu.nrows)
		return;
	int i = tmenu.rows[row];
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		o->menu.dirty = true;
	if (i < 0) {                                           /* wstecz: poziom wyżej */
		int32_t up = 0;
		for (int k = 0; k < tmenu.n; k++)
			if (tmenu.e[k].id == tmenu.parent)
				up = tmenu.e[k].parent;
		tmenu.parent = up;
		tmenu.hover = -1;
		tmenu_rows();
		river_window_manager_v1_manage_dirty(wm);
		return;
	}
	struct tmenu_entry *e = &tmenu.e[i];
	if (e->separator || !e->enabled)
		return;
	if (e->submenu) {                                      /* wejdź poziom niżej */
		sd_bus_call_method_async(bus, NULL, menu_service, menu_path, MENU_IFACE, "AboutToShow",
		                         NULL, NULL, "i", e->id);
		tmenu.parent = e->id;
		tmenu.hover = -1;
		tmenu_rows();
		river_window_manager_v1_manage_dirty(wm);
		return;
	}
	sd_bus_call_method_async(bus, NULL, menu_service, menu_path, MENU_IFACE, "Event", NULL, NULL,
	                         "isvu", e->id, "clicked", "i", 0, 0);
	sd_bus_flush(bus);
	tmenu_close();
}

/* Kliknięcie w ikonkę: lewy — Activate (albo menu, gdy ikona jest samym menu),
 * środkowy — SecondaryActivate, prawy — menu aplikacji (dbusmenu), a gdy go nie
 * ma — ContextMenu, czyli aplikacja otwiera własne okienko. */
void tray_click(int i, int button, int x, int y, struct output *o)
{
	if (!bus || i < 0 || i >= tray_n)
		return;
	struct tray_item *it = tray_items[i];
	bool want_menu = button == 3 || (button == 1 && it->item_is_menu);
	if (want_menu && tmenu.open && !strcmp(menu_service, it->service)) {
		tmenu_close();                                     /* drugi klik w tę samą ikonkę zamyka */
		return;
	}
	tmenu_close();
	menu_fallback.i = i;
	menu_fallback.x = x;
	menu_fallback.y = y;
	if (want_menu && it->menu)
		menu_open(it, x, o);
	else if (button == 3)
		context_menu_fallback();
	else
		sd_bus_call_method_async(bus, NULL, it->service, it->path, ITEM_IFACE,
		                         button == 1 ? "Activate" : "SecondaryActivate", NULL, NULL, "ii", x, y);
	sd_bus_flush(bus);
}
