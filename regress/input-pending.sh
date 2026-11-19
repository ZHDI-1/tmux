#!/bin/sh

. ./input-common.inc

FIFO="$TMP.fifo"
mkfifo "$FIFO" || exit 1
trap 'exec 3>&-; rm -f "$TMP" "$EXP" "$FIFO"; stop_server $TMUX || exit 1' 0 1 15

pending_matches()
{
	$TMUX capture-pane -pP -t pending: >"$TMP" || return 1
	cmp -s "$TMP" "$EXP"
}

check_pending()
{
	printf '%b\n' "$1" >"$EXP"
	wait_until pending_matches || fail 'pending input'
}

# Complete sequences in a read are discarded, but an unfinished suffix and
# its continuation in the next read must remain available to capture-pane -P.
start_cmd pending 20 3 "exec 3<'$FIFO';
	printf '\033[31mA\033[38;'; read step <&3;
	printf '2;1;2;3'; read step <&3;
	printf 'mB\033]2;split'; read step <&3;
	printf '%s' '-title'; read step <&3;
	printf '\007C'; $INPUT_HOLD"
exec 3>"$FIFO"

check_pending '\033[38;'
printf 'next\n' >&3
check_pending '\033[38;2;1;2;3'
printf 'next\n' >&3
check_pending '\033]2;split'
printf 'next\n' >&3
check_pending '\033]2;split-title'
printf 'next\n' >&3
wait_for_output split-title $TMUX display-message -p -t pending: \
    '#{pane_title}' || exit 1
check_pending ''
check_capture pending 'ABC'
check_raw_matches pending \
	'C 0,0 data=\(1,1,A\).* fg=red\[1\]' \
	'C 0,1 data=\(1,1,B\).* fg=#010203\[2010203\]'

exit $exit_status
