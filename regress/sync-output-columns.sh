#!/bin/sh

. ./regress-common.inc

# Compare the pane grid with an attached terminal after narrow synchronized
# updates, including wide-cell repairs and dirty columns moved by scrolling.

PATH=/bin:/usr/bin
TERM=screen
LC_ALL=C.UTF-8
export PATH TERM LC_ALL
[ -z "$TEST_TMUX" ] && TEST_TMUX=$(readlink -f ../tmux)
DIR=$(test_tmpdir) || exit 1
TMUX_TMPDIR=$DIR
export TMUX_TMPDIR
INNER="$TEST_TMUX -Li$$ -f/dev/null"
OUTER="$TEST_TMUX -Lo$$ -f/dev/null"

cleanup()
{
	stop_server $OUTER || exit 1
	stop_server $INNER || exit 1
	rm -rf "$DIR"
}
trap cleanup 0 1 15

cat >"$DIR/emitter.pl" <<'PERL'
use strict;
use warnings;
binmode STDIN;
binmode STDOUT;
my $dir = $ENV{CONTROL};
sub emit {
	my ($data) = @_;
	$data .= "\e[6n";
	while (length $data) {
		my $n = syswrite STDOUT, $data;
		die "write: $!\n" unless defined $n && $n;
		substr($data, 0, $n, '');
	}
	my $reply = '';
	while ($reply !~ /\e\[\d+;\d+R/) {
		my $n = sysread STDIN, my $ch, 1;
		die "read: $!\n" unless defined $n && $n;
		$reply .= $ch;
	}
}
sub ready {
	my ($stage) = @_;
	open my $fh, '>', "$dir/ready.$stage" or die "$!\n";
	close $fh;
}
my $initial = "\e[H\e[2J";
for my $row (1 .. 24) {
	$initial .= substr(sprintf("ROW%02d_", $row) . ('x' x 80), 0, 80);
}
emit($initial . "\e[1;1H");
ready(0);
my $wide = pack('H*', 'e7958ce7958c');
my @frames = (
	"\e[2;35H\e[31mABC\e[0m",
	"\e[2;10HLEFT\e[2;60HRIGHT",
	"\e[6;10H$wide\e[6;11Hx\e[6;14Hz",
	"\e[8;5H\e[48;2;10;40;90m      \e[0m\e[8;7H\e[3X",
	"\e[12;10H\e[4h+\e[4l\e[12;20H\e[3P",
	"\e[3;18r\e[8;40HMOVED\e[18;1H\r\n\e[r",
	"\e[20;80H!\e[21;1HNEXT",
);
for my $stage (1 .. scalar(@frames) + 1) {
	while (!-e "$dir/go.$stage") { select undef, undef, undef, 0.01; }
	if ($stage <= scalar @frames) {
		emit("\e[?2026h" . $frames[$stage - 1] . "\e[?2026l");
	} else {
		# Force a parser callback boundary while synchronization is active.
		emit("\e[?2026h\e[4;20HFIRST");
		emit("\e[4;60HLAST\e[?2026l");
	}
	ready($stage);
}
select undef, undef, undef, 30;
PERL

$INNER new-session -d -s inner -x 80 -y 24 \
    "stty raw -echo; CONTROL='$DIR' perl '$DIR/emitter.pl'" || exit 1
$INNER set-option -g status off || exit 1
$INNER set-option -g window-size manual || exit 1
$INNER set-option -as terminal-features '*:RGB:sync' || exit 1
$OUTER new-session -d -s outer -x 80 -y 24 \
    "$TEST_TMUX -Li$$ -f/dev/null attach-session -t inner" || exit 1
$OUTER set-option -g status off || exit 1
$OUTER set-option -g window-size manual || exit 1

stage=0
while [ "$stage" -le 8 ]; do
	[ "$stage" -eq 0 ] || : >"$DIR/go.$stage"
	tries=0
	while [ ! -e "$DIR/ready.$stage" ] && [ "$tries" -lt 100 ]; do
		sleep 0.05
		tries=$((tries + 1))
	done
	[ -e "$DIR/ready.$stage" ] || exit 1
	for format in text styled; do
		flag=
		[ "$format" = styled ] && flag=-e
		tries=0
		while [ "$tries" -lt 50 ]; do
			$INNER capture-pane -p $flag -t inner >"$DIR/expected" || exit 1
			$OUTER capture-pane -p $flag -t outer >"$DIR/actual" || exit 1
			cmp -s "$DIR/expected" "$DIR/actual" && break
			sleep 0.05
			tries=$((tries + 1))
		done
		if [ "$tries" -eq 50 ]; then
			echo "synchronized columns differ at stage $stage ($format)" >&2
			diff -u "$DIR/expected" "$DIR/actual" >&2
			exit 1
		fi
	done
	stage=$((stage + 1))
done
exit 0
