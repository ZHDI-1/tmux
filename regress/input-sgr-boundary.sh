#!/bin/sh

. ./input-common.inc

FIFO="$TMP.fifo"
mkfifo "$FIFO" || exit 1
trap 'exec 3>&-; rm -f "$TMP" "$EXP" "$FIFO"; stop_server $TMUX || exit 1' 0 1 15

pending_matches()
{
	$TMUX capture-pane -pP -t boundary: >"$TMP" || return 1
	cmp -s "$TMP" "$EXP"
}

# Every possible split of an ordinary SGR must retain the unfinished prefix
# and produce the same attributes when its continuation arrives.
seq=$(printf '\033[1;38;2;1;2;3;48;2;4;5;6m')
cut=1
while [ "$cut" -lt "${#seq}" ]; do
	left=$(printf '%s' "$seq" | cut -b "1-$cut")
	right=$(printf '%s' "$seq" | cut -b "$((cut + 1))-")
	start_cmd boundary 20 3 "exec 3<'$FIFO';
		printf '%s' '$left'; read step <&3;
		printf '%s' '$right'; printf 'X\\033]2;done\\007'; $INPUT_HOLD"
	exec 3>"$FIFO"
	printf '%s\n' "$left" >"$EXP"
	wait_until pending_matches || fail "SGR prefix at $cut"
	printf 'next\n' >&3
	wait_for_output done $TMUX display-message -p -t boundary: \
	    '#{pane_title}' || exit 1
	check_capture boundary 'X'
	check_raw_matches boundary \
	    'C 0,0 data=\(1,1,X\).* attr=BRIGHT\[1\] fg=#010203\[2010203\] bg=#040506\[2040506\]'
	exec 3>&-
	cut=$((cut + 1))
done

exit $exit_status
