# norns

A BSP window manager for the [river](https://codeberg.org/river/river) Wayland
compositor (river-window-management-v1), with its own bar, **urd**.

Every new window splits the focused tile in half along its longer side. Each
monitor has nine tags, each tag its own tree. Two looks are built in and
switch together — window borders, bar, wallpaper and terminal palette:
**GRUVBOX** and **MU/TH/UR** (faded phosphor green with a live starfield).

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
- urd stays sharp on fractionally scaled outputs (e.g. 1.25)
- urd: clickable tags, window title, network/WireGuard, CPU, RAM, temperature,
  battery, volume, clock, theme switch, power menu
- MU/TH/UR: green glow around the focused window, twinkling stars, a comet now
  and then; Super+A turns all motion off

## Requirements

river 0.4 or newer (river-protocols), wayland, wayland-protocols,
wayland-cursor, xkbcommon, fcft, pixman, a C11 compiler, make. Optional: `wpctl` (PipeWire) for volume,
`systemctl` for the power menu, foot as the terminal.

On Arch: `pacman -S river wayland wayland-protocols libxkbcommon fcft pixman tllist`

## Build and run

    make
    make install        # installs to ~/.local/bin/norns

Start it from river's init (`~/.config/river/init`), falling back to another
window manager if norns exits:

    #!/bin/sh
    kanshi &
    ~/.local/bin/norns >/tmp/norns.log 2>&1
    exec rhine

While hacking, `./run.sh` swaps the running window manager for `./norns` from
the source directory and falls back to rhine when it exits.

## Keys

| Keys | Action |
|---|---|
| Super+Return | terminal (per theme) |
| Super+B / Super+E | browser / file manager |
| Super+Q | close window |
| Super+H/J/K/L | focus left/down/up/right (across monitors) |
| Super+Ctrl+H/J/K/L | swap with the neighbour in that direction |
| Super+1…9 | show tag |
| Super+Ctrl+1…9 | send window to tag |
| Super+, / Super+. | focus previous / next monitor |
| Super+Ctrl+, / Super+Ctrl+. | send window to previous / next monitor |
| Super+F | toggle floating |
| Super+Ctrl+F | toggle fullscreen |
| Super+T | switch theme |
| Super+A | animations on/off |
| Super+left drag | move (floating) / swap or insert (tiles) |
| Super+right drag | resize (floating) / move split (tiles) |
| volume keys | volume via wpctl |
| Super+Esc | quit norns (river keeps running) |

## Configuration

Everything lives in `config.h`: gaps, borders, animation times, both themes,
programs, keys, application rules and windows that always float. Rebuild after
editing.

New windows are logged to `/tmp/norns.log` with their `app_id`, which is what
rules match on.

On theme change norns runs `~/.config/norns/theme <gruvbox|muthur>` if it is
executable — see `theme.example` for switching foot's palette. The chosen theme
is remembered in `~/.local/state/norns/theme`.

## License

MIT, see `LICENSE`.
