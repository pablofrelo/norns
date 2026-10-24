// sensors — odczyty dla paska urd z /proc i /sys (bez zewnętrznych bibliotek)
#define _DEFAULT_SOURCE
#include <dirent.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "norns.h"

struct sensors sens;

static bool read_line(const char *path, char *buf, size_t n)
{
	FILE *f = fopen(path, "r");
	if (!f)
		return false;
	bool ok = fgets(buf, n, f) != NULL;
	fclose(f);
	if (ok)
		buf[strcspn(buf, "\n")] = 0;
	return ok;
}

static long read_long(const char *path, long fallback)
{
	char buf[64];
	return read_line(path, buf, sizeof buf) ? strtol(buf, NULL, 10) : fallback;
}

/* Procent zajętości CPU od poprzedniego odczytu. */
static void update_cpu(void)
{
	static unsigned long long prev_idle, prev_total;
	unsigned long long v[8] = { 0 };
	FILE *f = fopen("/proc/stat", "r");
	if (!f)
		return;
	int n = fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
	               &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]);
	fclose(f);
	if (n < 4)
		return;
	unsigned long long idle = v[3] + v[4], total = 0;
	for (int i = 0; i < 8; i++)
		total += v[i];
	if (prev_total && total > prev_total)
		sens.cpu = (int)(100 * (1.0 - (double)(idle - prev_idle) / (total - prev_total)));
	prev_idle = idle;
	prev_total = total;
}

static void update_ram(void)
{
	FILE *f = fopen("/proc/meminfo", "r");
	if (!f)
		return;
	char key[32];
	unsigned long val, total = 0, avail = 0;
	while (fscanf(f, "%31s %lu kB\n", key, &val) == 2) {
		if (!strcmp(key, "MemTotal:"))
			total = val;
		else if (!strcmp(key, "MemAvailable:"))
			avail = val;
	}
	fclose(f);
	if (total)
		sens.ram_gb = (total - avail) / 1048576.0;
}

/* Czujnik CPU: k10temp (AMD), potem coretemp (Intel), potem cokolwiek z temp1. */
static void update_temp(void)
{
	static char path[300];
	static bool searched;
	if (!searched) {
		searched = true;
		const char *want[] = { "k10temp", "coretemp", NULL };
		glob_t g;
		if (glob("/sys/class/hwmon/hwmon*", 0, NULL, &g) == 0) {
			for (int w = 0; want[w] && !path[0]; w++)
				for (size_t i = 0; i < g.gl_pathc; i++) {
					char p[300], name[64];
					snprintf(p, sizeof p, "%s/name", g.gl_pathv[i]);
					if (read_line(p, name, sizeof name) && !strcmp(name, want[w])) {
						snprintf(path, sizeof path, "%s/temp1_input", g.gl_pathv[i]);
						break;
					}
				}
			globfree(&g);
		}
	}
	long t = path[0] ? read_long(path, -1) : -1;
	sens.has_temp = t >= 0;
	sens.temp = (int)(t / 1000);
}

static void update_bat(void)
{
	static char dir[300];
	static bool searched;
	if (!searched) {
		searched = true;
		glob_t g;
		if (glob("/sys/class/power_supply/BAT*", 0, NULL, &g) == 0) {
			snprintf(dir, sizeof dir, "%s", g.gl_pathv[0]);
			globfree(&g);
		}
	}
	sens.has_bat = false;
	if (!dir[0])
		return;
	char p[320], st[32];
	snprintf(p, sizeof p, "%s/capacity", dir);
	long cap = read_long(p, -1);
	if (cap < 0)
		return;
	snprintf(p, sizeof p, "%s/status", dir);
	if (!read_line(p, st, sizeof st))
		st[0] = 0;
	sens.has_bat = true;
	sens.bat = (int)cap;
	sens.bat_state = !strcmp(st, "Charging") ? '+' : !strcmp(st, "Discharging") ? '-' : '=';
}

/* LAN = fizyczny interfejs (ma device) bez wireless/, WIFI = z wireless/, WG = wg* istnieje. */
static void update_net(void)
{
	sens.lan = sens.wifi = sens.wg = false;
	DIR *d = opendir("/sys/class/net");
	if (!d)
		return;
	struct dirent *e;
	while ((e = readdir(d))) {
		const char *n = e->d_name;
		if (n[0] == '.' || !strcmp(n, "lo"))
			continue;
		if (!strncmp(n, "wg", 2)) {
			sens.wg = true;
			continue;
		}
		char p[300], state[16];
		snprintf(p, sizeof p, "/sys/class/net/%s/device", n);
		if (access(p, F_OK) != 0)
			continue;                       /* wirtualny (docker, bridge…) */
		snprintf(p, sizeof p, "/sys/class/net/%s/operstate", n);
		if (!read_line(p, state, sizeof state) || strcmp(state, "up"))
			continue;
		snprintf(p, sizeof p, "/sys/class/net/%s/wireless", n);
		if (access(p, F_OK) == 0)
			sens.wifi = true;
		else
			sens.lan = true;
	}
	closedir(d);
}

/* PipeWire: "Volume: 0.45" albo "Volume: 0.45 [MUTED]" */
void sensors_update_vol(void)
{
	sens.vol = -1;
	FILE *p = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
	if (!p)
		return;
	char buf[64] = "";
	float v;
	if (fgets(buf, sizeof buf, p) && sscanf(buf, "Volume: %f", &v) == 1) {
		sens.vol = (int)(v * 100 + 0.5f);
		sens.muted = strstr(buf, "MUTED") != NULL;
	}
	pclose(p);
}

void sensors_update(void)
{
	sensors_update_vol();
	update_cpu();
	update_ram();
	update_temp();
	update_bat();
	update_net();
}
