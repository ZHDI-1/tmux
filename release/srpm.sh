#!/usr/bin/env bash
set -euo pipefail
source_dir=$(realpath "$1")
cache=$(realpath "$2")
top=$(mkdir -p "$3" && realpath "$3")
here=$(cd -- "$(dirname -- "$0")" && pwd)
mkdir -p "$top/SOURCES" "$top/stage/tmux-io256-source/"{tmux,release,downloads}
stage=$top/stage/tmux-io256-source
git -C "$source_dir" archive HEAD | tar -xf - -C "$stage/tmux"
cp "$here"/{build.py,smoke.py,dependencies.json,ncurses-terminfo.patch,ghostty-terminfo.txt,INSTALL.md} "$stage/release/"
for archive in libevent.tar.gz utf8proc.tar.gz libpng.tar.xz zlib-ng.tar.gz ncurses.tar.gz; do
    cp "$cache/$archive" "$stage/downloads/"
done
tar -C "$top/stage" -czf "$top/SOURCES/tmux-io256-source.tar.gz" tmux-io256-source
sed -e "s/%{release_version}/$4/" -e "s/%{release_number}/$5/" "$here/tmux-io256.spec" > "$top/tmux-io256.spec"
rpmbuild -bs --nodeps --define "_topdir $top" "$top/tmux-io256.spec"
