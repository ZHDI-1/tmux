#!/bin/sh

. ./regress-common.inc

PATH=/bin:/usr/bin
TERM=screen
TZ=UTC
export PATH TERM TZ

[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
TMUX="$TEST_TMUX -Lformat-percent-$$ -f/dev/null"
trap 'stop_server $TMUX' 0

$TMUX new-session -d -s test || exit 1
$TMUX set -g @when 1700000000 || exit 1

[ "$($TMUX display-message -p '100%')" = '100%' ] || exit 1
[ "$($TMUX display-message -p '#{e|%%|:7,3}')" = 1 ] || exit 1
[ "$($TMUX list-windows -F '#{t/f/%Y 100%:@when}')" = '2023 100%' ] || exit 1
[ "$($TMUX list-windows -F '#{t/f/%Y %Q:@when}')" = '2023 %Q' ] || exit 1
[ "$($TMUX list-windows -F '#{t/f/%%:@when}')" = '%' ] || exit 1

exit 0
