// notify — demon powiadomień (org.freedesktop.Notifications) na magistrali zasobnika.
//
// Powiadomienia rysuje urd.c w prawym górnym rogu monitora z fokusem: tekst
// wpisywany znak po znaku jak na terminalu MU/TH/UR. Zwykłe znikają same,
// krytyczne wiszą do kliknięcia. Klik zamyka powiadomienie i uruchamia jego
// domyślną akcję (np. otwiera rozmowę).
#define _GNU_SOURCE
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <systemd/sd-bus.h>
#include "norns.h"

#define NOTE_PATH  "/org/freedesktop/Notifications"
#define NOTE_IFACE "org.freedesktop.Notifications"

struct note notes[NOTE_MAX];
int notes_n;
static sd_bus *bus;
static uint32_t next_id = 1;

static void copy(char *dst, size_t n, const char *src)
{
	snprintf(dst, n, "%s", src ? src : "");
	for (char *p = dst; *p; p++)                /* jedna linia na nagłówek; \n w treści zostaje */
		if (*p == '\t' || *p == '\r')
			*p = ' ';
}

/* Treść bez znaczników <b>, <i>, <a href…> i z rozwiniętymi &amp; &lt; &gt;. */
static void strip_markup(char *s)
{
	char *r = s, *w = s;
	while (*r) {
		if (*r == '<') {
			char *e = strchr(r, '>');
			if (e) {
				r = e + 1;
				continue;
			}
		}
		if (*r == '&') {
			static const struct { const char *e; char c; } ents[] = {
				{ "&amp;", '&' }, { "&lt;", '<' }, { "&gt;", '>' }, { "&quot;", '"' }, { "&apos;", '\'' },
			};
			bool hit = false;
			for (size_t i = 0; i < sizeof ents / sizeof ents[0]; i++)
				if (!strncmp(r, ents[i].e, strlen(ents[i].e))) {
					*w++ = ents[i].c;
					r += strlen(ents[i].e);
					hit = true;
					break;
				}
			if (hit)
				continue;
		}
		*w++ = *r++;
	}
	*w = 0;
}

static void changed(void)
{
	struct output *o;
	wl_list_for_each(o, &outputs, link)
		o->note.dirty = true;
	river_window_manager_v1_manage_dirty(wm);   /* przerysuj w najbliższej sekwencji */
}

static void drop(int i, uint32_t reason)
{
	if (bus)
		sd_bus_emit_signal(bus, NOTE_PATH, NOTE_IFACE, "NotificationClosed", "uu", notes[i].id, reason);
	memmove(&notes[i], &notes[i + 1], (notes_n - i - 1) * sizeof notes[0]);
	notes_n--;
	changed();
}

static int m_notify(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	const char *app, *icon, *summary, *body, *s;
	uint32_t replaces;
	int32_t timeout;
	uint8_t urgency = 1;
	bool has_default = false;

	if (sd_bus_message_read(m, "susss", &app, &replaces, &icon, &summary, &body) < 0)
		return -EINVAL;
	if (sd_bus_message_enter_container(m, 'a', "s") >= 0) {     /* akcje: pary klucz, opis */
		bool key = true;
		while (sd_bus_message_read(m, "s", &s) > 0) {
			if (key && !strcmp(s, "default"))
				has_default = true;
			key = !key;
		}
		sd_bus_message_exit_container(m);
	}
	if (sd_bus_message_enter_container(m, 'a', "{sv}") >= 0) {   /* podpowiedzi: tylko pilność */
		while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
			if (sd_bus_message_read(m, "s", &s) >= 0 && !strcmp(s, "urgency") &&
			    sd_bus_message_enter_container(m, 'v', "y") >= 0) {
				sd_bus_message_read(m, "y", &urgency);
				sd_bus_message_exit_container(m);
			} else {
				sd_bus_message_skip(m, "v");
			}
			sd_bus_message_exit_container(m);
		}
		sd_bus_message_exit_container(m);
	}
	if (sd_bus_message_read(m, "i", &timeout) < 0)
		timeout = -1;

	int i;
	for (i = 0; i < notes_n; i++)
		if (replaces && notes[i].id == replaces)
			break;
	if (i == notes_n) {                         /* nowe: na górę, najstarsze wypada */
		if (notes_n == NOTE_MAX)
			drop(NOTE_MAX - 1, 1);
		memmove(&notes[1], &notes[0], notes_n * sizeof notes[0]);
		notes_n++;
		i = 0;
		notes[0].id = replaces ? replaces : next_id++;
	}
	struct note *n = &notes[i];
	copy(n->app, sizeof n->app, app);
	copy(n->summary, sizeof n->summary, summary);
	copy(n->body, sizeof n->body, body);
	strip_markup(n->summary);
	strip_markup(n->body);
	n->urgency = urgency;
	n->has_default = has_default;
	n->t0 = now_ms();
	n->expire = urgency == 2 || timeout == 0 ? 0 : n->t0 + (timeout > 0 ? timeout : NOTE_MS);
	fprintf(stderr, "norns: powiadomienie %u od %s\n", n->id, n->app);
	changed();
	return sd_bus_reply_method_return(m, "u", n->id);
}

static int m_close(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	uint32_t id;
	if (sd_bus_message_read(m, "u", &id) < 0)
		return -EINVAL;
	for (int i = 0; i < notes_n; i++)
		if (notes[i].id == id) {
			drop(i, 3);
			break;
		}
	return sd_bus_reply_method_return(m, "");
}

static int m_caps(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	sd_bus_message *r;
	sd_bus_message_new_method_return(m, &r);
	sd_bus_message_append(r, "as", 2, "body", "actions");
	int ret = sd_bus_send(NULL, r, NULL);
	sd_bus_message_unref(r);
	return ret;
}

static int m_info(sd_bus_message *m, void *ud, sd_bus_error *err)
{
	return sd_bus_reply_method_return(m, "ssss", "norns", "norns", "0.1", "1.2");
}

static const sd_bus_vtable note_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("Notify", "susssasa{sv}i", "u", m_notify, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("CloseNotification", "u", "", m_close, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetCapabilities", "", "as", m_caps, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("GetServerInformation", "", "ssss", m_info, SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("NotificationClosed", "uu", 0),
	SD_BUS_SIGNAL("ActionInvoked", "us", 0),
	SD_BUS_VTABLE_END,
};

void notify_init(sd_bus *b)
{
	if (sd_bus_add_object_vtable(b, NULL, NOTE_PATH, NOTE_IFACE, note_vtable, NULL) < 0 ||
	    sd_bus_request_name(b, NOTE_IFACE, 0) < 0) {
		fprintf(stderr, "norns: powiadomienia — działa już inny demon (mako, dunst?)\n");
		return;
	}
	bus = b;
	fprintf(stderr, "norns: powiadomienia — jestem demonem\n");
}

/* Co 2 s z pętli: przeterminowane znikają. */
void notify_tick(void)
{
	uint64_t now = now_ms();
	for (int i = notes_n - 1; i >= 0; i--)
		if (notes[i].expire && now >= notes[i].expire)
			drop(i, 1);
}

/* Trwa wpisywanie tekstu — potrzebne kolejne klatki. */
bool notify_busy(uint64_t now)
{
	for (int i = 0; i < notes_n; i++) {
		size_t len = strlen(notes[i].summary) + strlen(notes[i].body);
		if (now < notes[i].t0 + note_type_ms(len))
			return true;
	}
	return false;
}

int note_type_ms(size_t chars)
{
	int ms = (int)chars * NOTE_CHAR_MS;
	return ms < NOTE_TYPE_MAX ? ms : NOTE_TYPE_MAX;
}

/* Klik w powiadomienie: domyślna akcja (jeśli jest) i zamknięcie. */
void notify_click(int i)
{
	if (i < 0 || i >= notes_n)
		return;
	if (bus && notes[i].has_default)
		sd_bus_emit_signal(bus, NOTE_PATH, NOTE_IFACE, "ActionInvoked", "us", notes[i].id, "default");
	drop(i, 2);
	if (bus)
		sd_bus_flush(bus);
}
