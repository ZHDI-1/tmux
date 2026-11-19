#!/usr/bin/env bash
set -euo pipefail
export LC_ALL=C
source_dir=$1
output=$2
tag=$3
target=$4
here=$(cd -- "$(dirname -- "$0")" && pwd)
base=tmux-io256-$tag-$target
mkdir -p "$output/$base"
cp "$source_dir/tmux" "$output/$base/tmux"
cp "$source_dir/tmux.1" "$output/$base/tmux.1"
cp "$here/INSTALL.md" "$output/$base/README.md"
cp "$here/dependencies.json" "$output/$base/dependencies.json"
mkdir -p "$output/$base/licenses"
cp "$source_dir/COPYING" "$output/$base/licenses/tmux.txt"
cp "$here/ghostty-terminfo.txt" "$output/$base/licenses/"
work=$5
cp "$work"/libevent-*/LICENSE "$output/$base/licenses/libevent.txt"
cp "$work"/utf8proc-*/LICENSE.md "$output/$base/licenses/utf8proc.txt"
cp "$work"/libpng-*/LICENSE "$output/$base/licenses/libpng.txt"
cp "$work"/zlib-ng-*/LICENSE.md "$output/$base/licenses/zlib-ng.txt"
cp "$work"/ncurses-*/COPYING "$output/$base/licenses/ncurses.txt"
if [[ $target == linux-* ]]; then
    strip --strip-debug "$output/$base/tmux"
    mkdir "$output/$base/locale"
    localedef --no-archive -i C -f UTF-8 "$output/$base/locale/C.utf8"
    cp /usr/share/doc/libc6/copyright "$output/$base/licenses/glibc.txt"
    dpkg-query -W libc6 libc6-dev locales > "$output/$base/toolchain.txt"
    gcc --version >> "$output/$base/toolchain.txt"
else
    strip -S "$output/$base/tmux"
    codesign --force --sign - "$output/$base/tmux"
    codesign --verify "$output/$base/tmux"
    clang --version > "$output/$base/toolchain.txt"
fi
git -C "$source_dir" rev-parse HEAD > "$output/$base/source-commit.txt"
python3 "$here/smoke.py" "$output/$base/tmux"
tar -C "$output" -czf "$output/$base.tar.gz" "$base"
