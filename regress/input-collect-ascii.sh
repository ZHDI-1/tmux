#!/bin/sh

. ./input-common.inc

# A collected run must leave wrapping to the character after the right edge.
start_pane wrapped-run 5 3 'ABCDEFGHIJKL'
check_capture wrapped-run 'ABCDE
FGHIJ
KL'
check_cursor wrapped-run '2,2'
check_flags wrapped-run 'W ABCDE
W FGHIJ
- KL'

# With wrapping disabled, a one-column screen must keep overwriting its cell.
start_pane single-column 1 3 '\033[?7lABCDE'
check_capture single-column 'E'
check_cursor single-column '0,0'

# REP must use the final character of the preceding run.
start_pane repeat-run 8 3 'abc\033[3b'
check_capture repeat-run 'abcccc'
check_cursor repeat-run '6,0'

# ASCII after an unfinished UTF-8 character must first emit a replacement.
start_pane interrupted-utf8 8 3 '\303ABC'
check_capture interrupted-utf8 '�ABC'

# A run starting inside wide-character padding must clear its old leading cell.
start_pane padding-run 8 3 '\347\225\214AB\r\033[CXZ'
check_capture padding-run ' XZB'
check_cursor padding-run '3,0'

exit $exit_status
