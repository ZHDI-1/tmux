#!/bin/sh

. ./input-common.inc

# A wide glyph that cannot fit the last column wraps as a whole.
start_pane wide-wrap 5 3 '\347\225\214\347\225\214\347\225\214\347\225\214\347\225\214'
check_capture wide-wrap '界界
界界
界'
check_cursor wide-wrap '2,2'
check_flags wide-wrap 'WX 界界
WX 界界
X 界'

# REP must retain the final complete wide glyph, including its width.
start_pane wide-repeat 12 3 '\303\251\347\225\214\033[2b'
check_capture wide-repeat 'é界界界'
check_cursor wide-repeat '7,0'

# A combining mark after an ordinary run still joins the preceding glyph.
start_pane combining-tail 12 3 '\303\251\316\251\347\225\214\314\201'
check_capture combining-tail 'éΩ界́'
check_cursor combining-tail '4,0'

exit $exit_status
