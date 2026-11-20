%global debug_package %{nil}

Name:           tmux
Version:        %{release_version}
Release:        %{release_number}%{?dist}
Summary:        tmux terminal multiplexer with the io256 PTY reader
License:        ISC AND BSD-3-Clause AND BSD-2-Clause AND Zlib AND libpng-2.0 AND LGPL-2.1-or-later AND MIT AND Unicode-3.0
URL:            https://github.com/ZHDI-1/tmux
Source0:        tmux-io256-source.tar.gz
ExclusiveArch:  x86_64 aarch64
BuildRequires:  autoconf
BuildRequires:  automake
BuildRequires:  binutils
BuildRequires:  bison
BuildRequires:  cmake
BuildRequires:  gcc
BuildRequires:  gcc-c++
BuildRequires:  glibc-static
BuildRequires:  make
BuildRequires:  ncurses
BuildRequires:  patch
BuildRequires:  pkgconf-pkg-config
BuildRequires:  python3
BuildRequires:  systemd-rpm-macros
Requires:       glibc-minimal-langpack
Obsoletes:      tmux-io256 < %{version}-%{release}

%description
The io256 tmux fork, linked statically with glibc and its third-party
libraries. This package replaces the Fedora tmux package.

%prep
%setup -q -n tmux-io256-source

%build
python3 release/build.py --offline --source tmux --cache downloads --work deps

%install
install -D -m 0755 tmux/tmux %{buildroot}%{_bindir}/tmux
install -D -m 0644 tmux/tmux.1 %{buildroot}%{_mandir}/man1/tmux.1
install -D -m 0644 release/tmux@.service %{buildroot}%{_unitdir}/tmux@.service
mkdir licenses
cp tmux/COPYING licenses/tmux.txt
cp release/ghostty-terminfo.txt licenses/
cp deps/libevent-*/LICENSE licenses/libevent.txt
cp deps/utf8proc-*/LICENSE.md licenses/utf8proc.txt
cp deps/libpng-*/LICENSE licenses/libpng.txt
cp deps/zlib-ng-*/LICENSE.md licenses/zlib-ng.txt
cp deps/ncurses-*/COPYING licenses/ncurses.txt

%check
python3 release/smoke.py %{buildroot}%{_bindir}/tmux

%files
%license licenses/*
%doc release/INSTALL.md tmux/CHANGES tmux/README tmux/example_tmux.conf
%{_bindir}/tmux
%{_mandir}/man1/tmux.1*
%{_unitdir}/tmux@.service
