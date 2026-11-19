#!/bin/sh

. ./input-common.inc

# Scrolling within a region must retain the row outside it.
lines=''
i=0
while [ "$i" -lt 100 ]; do
	lines="${lines}X\r\n"
	i=$((i + 1))
done
start_pane region-lines 6 3 "A\033[2;3r\033[3;1H${lines}Z"
check_capture region-lines 'A
X
Z'
check_cursor region-lines '1,2'

# Repeated complete lines must preserve a short history through trimming.
start_pane_hlimit history-lines 6 3 "${lines}Z" 7
check_capture history-lines 'X
X
Z'
actual=$($TMUX display-message -p -t history-lines: '#{history_size}')
[ "$actual" = 7 ] || fail 'history-lines history size'
$TMUX capture-pane -pN -t history-lines: -S - -E - | normalize_capture >"$TMP"
{ i=0; while [ "$i" -lt 9 ]; do printf 'X\n'; i=$((i + 1)); done; printf 'Z\n'; } >"$EXP"
cmp "$TMP" "$EXP" || fail 'history-lines retained rows'

exit $exit_status
