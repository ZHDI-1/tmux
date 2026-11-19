#!/bin/sh

. ./input-common.inc

# Missing and zero coordinates both use CUP's default; extra values are ignored.
start_pane cup-defaults 10 3 'A\033[;5HX\033[0;0fB\033[2;3;99HC'
check_capture cup-defaults 'B   X
  C'
check_cursor cup-defaults '3,1'

# Overflow must fall back to the normal invalid-command behavior.
start_pane cup-overflow 10 3 'A\033[2147483648;2HZ'
check_capture cup-overflow 'AZ'
check_cursor cup-overflow '2,0'

# SGR after a colon parameter must release its old string and apply RGB values.
start_pane numeric-after-colon 10 3 '\033[4:3m\033[38;2;1;2;3mX'
check_capture numeric-after-colon 'X'
check_raw_matches numeric-after-colon 'C 0,0 data=\(1,1,X\).*fg=#010203\[2010203\]'

exit $exit_status
