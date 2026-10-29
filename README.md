# norns

*Not Only a River's Next Sloppy WM*

A BSP window manager for the [river](https://codeberg.org/river/river) Wayland
compositor (river-window-management-v1), with its own bar, **urd**.

Every new window splits the focused tile in half along its longer side. Each
monitor has nine tags, each tag its own tree. Two looks are built in and
switch together — window borders, bar, wallpaper and terminal palette:
**GRUVBOX**, **MU/TH/UR** (faded phosphor green with a live starfield), and two
calm ones: **GRAFIT** (near-monochrome graphite with a muted sage accent, the zen
theme) and **PAPIER** (light, pastel paper).

The code was written by Claude (Anthropic) under my direction and is tested
and used daily on my own machine.

## Features

- BSP tiling with adjustable split ratios, gaps, focus follows mouse
- nine tags per monitor, sliding animation between them
- mouse: drag a tile onto another to swap, onto its edge to insert next to it,
  onto an empty monitor to move it there; resize splits with Super+right drag
- floating dialogs centred over their parent, Super+F to float any window
- fullscreen (application requests and a key binding)
- per-application rules: open an app on a chosen monitor, matched by serial
  number, not by the changing DP-n connector name
- the layout survives a norns restart (`./run.sh`, a detour through another
  window manager): windows return to their monitor, tag and place in the tree,
  floating ones to their position; kept in `$XDG_RUNTIME_DIR/norns/layout`
- urd stays sharp on fractionally scaled outputs (e.g. 1.25)
- urd: clickable tags, window title, network/WireGuard, CPU, RAM, temperature,
  battery, volume, system tray (StatusNotifierItem: left click activates, middle
  is the secondary action, right click opens the application's menu — drawn by
  urd from com.canonical.dbusmenu, e.g. nm-applet's network list), clock, theme switch (the power menu is on Super+Shift+Escape)
- screen lock: swaylock in the colours of the current theme by default, any
  ext-session-lock-v1 locker can be set in `config.h`
- verdandi, the built-in notification daemon (org.freedesktop.Notifications): notifications
  type themselves out at the top centre of every monitor; critical
  ones stay until clicked, a click runs the default action (if another daemon
  such as mako is already running, norns leaves it alone)
- zen mode (Super+Z, on every monitor at once): the GRAFIT theme, no bar, gaps, borders or glow;
  notifications wait until you leave zen and norns' own sounds go quiet —
  the keyboard works as usual
- screen saver built in: BSP tiles that keep splitting in half and folding back,
  the way norns lays out windows, in the colours of the current theme
  (`norns-ctl saver`; mouse movement, a click or any norns key wakes it)
- MU/TH/UR: green glow around the focused window, twinkling stars, a comet now
  and then; Super+A turns all motion off

## Requirements

river 0.4 or newer (river-protocols), wayland, wayland-protocols,
wayland-cursor, xkbcommon, fcft, pixman, libsystemd (sd-bus, for the tray),
libpng, a C11 compiler, make. Optional: `wpctl` (PipeWire) for volume,
`systemctl` for the power menu, `swaylock` for the screen lock, foot as the terminal.

On Arch: `pacman -S river wayland wayland-protocols libxkbcommon fcft pixman tllist`

## Build and run

    make
    make install        # installs to ~/.local/bin/norns

Start it from river's init (`~/.config/river/init`), falling back to another
window manager if norns exits:

    #!/bin/sh
    kanshi &
    ~/.local/bin/norns >/tmp/norns.log 2>&1
    exec rhine          # or any other river window manager you have

While hacking, `./run.sh` swaps the running window manager for `./norns` from
the source directory. When it exits, the window manager named in
`NORNS_FALLBACK` takes over ([rhine](https://codeberg.org/sivecano/rhine) by default, if installed), so the session
survives a crash:

    NORNS_FALLBACK=my-other-wm ./run.sh

## Keys

| Keys | Action |
|---|---|
| Super+Return | terminal (per theme) |
| Super+` | slide-up terminal (the same key hides it) |
| Super+D | app launcher (fuzzel, per theme) |
| Super+/ or Super+F1 | shortcut cheat sheet (fuzzel) |
| Print / Shift+Print / Super+Print | screenshot of the monitor / a region / the focused window (grim, slurp; saved to ~/Pictures/Screenshots and copied) |
| Super+B / Super+E | browser / file manager |
| Super+Q | close window |
| Super+Shift+L | lock the screen (swaylock by default, see `config.h`) |
| Super+Shift+Escape | power menu: lock / log out / reboot / power off (fuzzel) |
| Super+H/J/K/L | focus left/down/up/right (across monitors) |
| Super+Ctrl+H/J/K/L | swap with the neighbour in that direction |
| Super+1…9 | show tag |
| Super+Ctrl+1…9 | send window to tag |
| Super+, / Super+. | focus previous / next monitor |
| Super+Ctrl+, / Super+Ctrl+. | send window to previous / next monitor |
| Super+F | toggle floating |
| Super+Ctrl+F | toggle fullscreen |
| Super+T | next theme (gruvbox, muthur, grafit, papier) |
| Super+A | animations on/off |
| Super+Ctrl+A | CRT scanlines on/off |
| Super+Z | zen mode on/off |
| Super+left drag | move (floating) / swap or insert (tiles) |
| Super+right drag | resize (floating) / move split (tiles) |
| volume keys | volume via wpctl |
| Super+Esc | quit norns (river keeps running) |

## norns-ctl

Scripts and other programs can drive norns over D-Bus (installed with
`make install`):

    norns-ctl status            # theme=muthur anim=on ... DP-9:1:12---6---
    norns-ctl view 3            # show tag 3 on the monitor under the pointer
    norns-ctl send 3            # send the focused window to tag 3
    norns-ctl output next       # focus the next monitor
    norns-ctl theme papier      # gruvbox, muthur, grafit, papier, or toggle
    norns-ctl anim off          # on, off, or toggle when omitted
    norns-ctl crt on            # CRT scanlines
    norns-ctl saver on          # screen saver (off, or toggle when omitted)
    norns-ctl zen               # zen mode: on, off, or toggle when omitted
    norns-ctl drop              # slide-up terminal
    norns-ctl lock              # lock the screen
    norns-ctl power poweroff    # or logout, reboot

`norns-ctl` with no arguments lists every command.

## Configuration

Everything lives in `config.h`: gaps, borders, animation times, the themes
(and which one zen switches to, `ZEN_THEME`),
programs, keys, application rules and windows that always float. Rebuild after
editing.

New windows are logged to `/tmp/norns.log` with their `app_id`, which is what
rules match on.

On theme change norns runs `~/.config/norns/theme <theme name>` if it is
executable — see `theme.example` for switching foot's palette. The chosen theme
is remembered in `~/.local/state/norns/theme`.

The screen locker is set per theme in `config.h` (`lock_gruvbox`,
`lock_muthur`, …); swaylock by default, any ext-session-lock-v1 locker works.
norns does not watch for idle time by itself — for the screen saver and the
lock, add swayidle to river's init (saver after 10 minutes, lock after 20):

    swayidle -w timeout 600 'norns-ctl saver on' resume 'norns-ctl saver off' \
                timeout 1200 'norns-ctl lock' &

Saver timing (split interval, animation, number and minimum size of tiles) is in
`config.h` (`SAVER_*`).

## License

MIT, see `LICENSE`.
