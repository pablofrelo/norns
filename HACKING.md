# HACKING — mapa norns

Krótka mapa dla każdego, kto siada do kodu:
co gdzie leży, jak to się kręci i na co uważać. Komentarze w kodzie i opisy
commitów są po polsku, README po angielsku.

## Pliki

| plik | co robi |
|---|---|
| `norns.c` | WM: drzewa BSP, tagi, monitory, fokus, skróty (`run()`), mysz (przesuwanie/rozmiar), reguły, pełny ekran, terminal wysuwany, zrzuty, wyłącznik, norns-ctl (D-Bus), zapis/odtwarzanie układu, pętla `main()` |
| `norns.h` | wspólne struktury (`win`, `output`, `node`, `surf`, `tray_item`, `note`, `tmenu`) i deklaracje |
| `config.h` | stałe (`GAP`, `BORDER`, czasy animacji, rozmiary) oraz tablice: motywy, programy, `keys[]`, `help_lines[]`, `rules[]`, `float_app_ids[]` |
| `urd.c` | pasek (tagi, tytuł, sensory, zasobnik, zegar, motyw), tapeta (gwiazdy, kometa), powiadomienia i menu zasobnika — rysowanie; kliki myszy na własnych powierzchniach |
| `anim.c` | ślizg tagów, dojazd okien, otwieranie jak kineskop, ślizg terminala wysuwanego |
| `deco.c` | poświata / cień wokół okien (4 paski dekoracji) |
| `tray.c` | zasobnik: StatusNotifierWatcher/Host po sd-bus, ikony (pixmapy, PNG), dbusmenu |
| `verdandi.c` | demon powiadomień `org.freedesktop.Notifications` (treść; rysuje urd.c) |
| `fx.c` | warstwa efektów nad ekranem: linie kineskopu i wygaszacz z kafli BSP (własny budzik w `main()`, rysuje poza sekwencjami) — miejsce na kolejne efekty |
| `sfx.c` | dźwięki generowane w C (serwo terminala, migawka, stukot klawiszy), odtwarzane przez `pw-play` |
| `sensors.c` | CPU, RAM, temperatura, bateria, sieć, głośność (co 2 s) |
| `shm.c` | bufory `wl_shm` (`buf_new/attach/drop`, jednorazowe i wielokrotne) |
| `norns-ctl` | skrypt: `busctl` → metoda `Run(s)` w norns |

## Jak to się kręci (river-window-management-v1)

- River prowadzi WM przez **sekwencje**: `manage_start` → (decyzje: fokus,
  `propose_dimensions`, pełny ekran, nowe okna) → `manage_finish`, potem
  `render_start` → (pozycje, przycięcia, kolejność, ramki, pasek) →
  `render_finish`. Requesty tylko w swojej sekwencji (treść naszych
  powierzchni — tapeta, wygaszacz — można commitować też poza nimi).
- Zdarzenia (klawisze, D-Bus, timery) zmieniają tylko nasze dane i wołają
  `river_window_manager_v1_manage_dirty(wm)`; resztę robi następna sekwencja.
- `propose_dimensions` wysyłamy **tylko przy zmianie** (`w->pw/ph`) — każdy
  propose to przerysowanie okna. Gdy coś ma wymusić nowy rozmiar, ustaw
  `pw = ph = -1`.
- Nasze powierzchnie (pasek, tapeta, powiadomienia, menu, efekty) to
  `river_shell_surface_v1`; commit w render z `sync_next_commit`.
- Pętla `main()`: `poll` na Wayland, timer 2 s (sensory,
  powiadomienia), timer animacji 16 ms, timer tapety, timer wygaszacza, fd D-Bus.

## Rzeczy, o które łatwo się potknąć

- **Skala ułamkowa** (laptop 1,25): pasek, powiadomienia i menu rysowane są
  w pikselach bufora (`surf.scale120`) i wyświetlane przez viewporter.
  Granice klików trzymamy w pikselach bufora, kursor jest w logicznych.
- **Monitory**: wlroots (river) w `wl_output.geometry` zawsze daje 0,0 —
  położenie bierzemy z river (`output.x/y`). Monitory rozpoznajemy po
  opisie z numerem seryjnym (`desc_of`), nie po nazwach DP-n.
- **Monitor docelowy** dla wszystkiego, co otwiera się „tutaj” (fuzzel,
  wyłącznik, terminal wysuwany, zrzut monitora) = monitor pod kursorem
  (`active_output()`), nie okno z fokusem.
- **Dzieci procesu**: `SIGCHLD = SIG_IGN` — nie używać `wait()`; `SIGPIPE`
  ignorowany (ściąga pisze do fuzzela przez rurę).
- **glibc ma makro `CTRL(x)`** — config.h robi `#undef CTRL`.
- **Powiadomienia**: nazwę `org.freedesktop.Notifications` zajmujemy
  w kolejce; jeśli działa mako, przejmujemy po jego zakończeniu.
- **foot zaokrągla pływające okna** do pełnych znaków — środek liczymy
  z prawdziwego rozmiaru (`dw/dh`), nie z zaproponowanego.
- **Układ** zapisuje się do `$XDG_RUNTIME_DIR/norns/layout` po każdej
  sekwencji manage; okna po `river_window_v1.identifier`.

## Dodawanie rzeczy

- **Skrót**: wpis w `keys[]` (config.h), w razie potrzeby nowa wartość w
  `enum action` (norns.h) i `case` w `run()` (norns.c), linia w
  `help_lines[]` i w tabeli Keys w README.
- **Polecenie norns-ctl**: gałąź w `ctl_run()` (norns.c) + lista w
  komunikacie pomocy + przykład w README.
- **Dźwięk**: generator w sfx.c, plik w `$XDG_RUNTIME_DIR/norns/`
  tworzony w `sfx_init()`, odtwarzanie przez `pw-play`.
- **Kolor/czcionka**: tylko przez motyw (`THEME->...`), nigdy na sztywno.

## Budowanie i testy

    make && make install        # ~/.local/bin/norns i norns-ctl
    ./run.sh                    # (lokalny skrypt) podmienia działający WM na ./norns

- Log: `/tmp/norns.log` (nowe okna z app_id, zasobnik, powiadomienia,
  odtworzony układ). Czyszczony przy każdym starcie norns.
- Bez rivera da się testować logikę: pliki .c kompiluje się z atrapą
  `wl_proxy_marshal_flags` (`-Wl,--wrap=...`), D-Bus na prywatnej
  magistrali (`dbus-run-session`), rysowanie do obrazka (pixman → PPM).
