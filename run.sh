#!/bin/sh
# Test: podmienia działający WM na ./norns z katalogu projektu. Gdy ./norns
# się zakończy (albo padnie), sesję przejmuje NORNS_FALLBACK — domyślnie
# rhine, jeśli jest zainstalowany — żeby river nie został bez WM.
# Log: /tmp/norns.log
cd "$(dirname "$0")" || exit 1
fb=${NORNS_FALLBACK:-rhine}
pkill -x norns && sleep 1
pkill -x "$(basename "$fb")" && sleep 1
if command -v "$fb" >/dev/null 2>&1; then
	setsid -f sh -c './norns >/tmp/norns.log 2>&1; exec "$1" >/tmp/norns-fallback.log 2>&1' sh "$fb"
else
	echo "run.sh: brak $fb — bez zapasowego WM (ustaw NORNS_FALLBACK)" >&2
	setsid -f sh -c './norns >/tmp/norns.log 2>&1'
fi
