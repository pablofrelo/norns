#!/bin/sh
# Podmienia bieżący WM na norns. Gdy norns się zakończy (Super+Esc, crash,
# cokolwiek), automatycznie wraca rhine. Log: /tmp/norns.log
cd "$(dirname "$0")" || exit 1
pkill -x norns; pkill -x rhine; sleep 1
setsid -f sh -c './norns >/tmp/norns.log 2>&1; exec rhine >/tmp/rhine.log 2>&1'
