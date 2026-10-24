#!/bin/sh
# Test: podmienia WM na ./norns z katalogu projektu. Gdy ./norns się
# zakończy, wraca rhine. Log: /tmp/norns.log
cd "$(dirname "$0")" || exit 1
pkill -x norns && sleep 1      # sesyjny norns -> init przechodzi w tryb awaryjny
pkill -x rhine; pkill -x ironbar; sleep 1
setsid -f sh -c './norns >/tmp/norns.log 2>&1; exec ~/.local/bin/rhine >/tmp/rhine.log 2>&1'
