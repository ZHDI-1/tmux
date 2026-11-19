#!/bin/sh

. ./input-common.inc

# A no-op CUP must retain implicit blank-cell flags at an allocation boundary.
start_pane quarter-boundary 8 3 'A\033[1;2HB'
check_capture quarter-boundary 'AB'
check_cursor quarter-boundary '2,0'
check_raw_matches quarter-boundary \
    'C 0,2 data=\(1,1, \) flags=NONE\[0\]' \
    'C 0,3 data=\(1,1, \) flags=NONE\[0\]'

# The same applies at the half-row boundary and for the H/f aliases.
start_pane half-boundary 8 3 'A\033[1;2fB\033[1;3HC\033[1;4fD'
check_capture half-boundary 'ABCD'
check_cursor half-boundary '4,0'
check_raw_matches half-boundary \
    'C 0,4 data=\(1,1, \) flags=NONE\[0\]' \
    'C 0,7 data=\(1,1, \) flags=NONE\[0\]'

# Extended foreground storage must not expose extra cleared compact cells.
start_pane rgb-boundary 8 3 '\033[38;2;1;2;3mA\033[1;2HB'
check_capture rgb-boundary 'AB'
check_raw_matches rgb-boundary 'C 0,2 data=\(1,1, \) flags=NONE\[0\]'

# An explicit move to the last column cancels delayed wrapping.
start_pane cancel-wrap 4 3 'ABCD\033[1;4HX'
check_capture cancel-wrap 'ABCX'
check_cursor cancel-wrap '4,0'
check_flags cancel-wrap '- ABCX'

exit $exit_status
