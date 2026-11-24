# tmux io256

The release pin in the fork identifies the source revision. This release builds
on the io256 alpha (`tmux -V` reports `next-3.9-alpha1-io256`) and adds split
flat-viewport / persistent-radix scrollback storage, out-of-place width reflow,
and opt-in per-pane workers (`TMUX_PANE_WORKERS=1`). The Linux executables link
glibc and the third-party dependencies statically. The macOS executables use
Apple's system libraries and include the third-party dependencies, including
jemalloc. The macOS deployment target is 15.

Start a separate server from an extracted archive:

    ./tmux -L io256 new-session

On Linux, if the system lacks a UTF-8 locale, point glibc at the bundled data:

    LOCPATH="$PWD/locale" ./tmux -L io256 new-session

The Linux binary uses the host's ordinary name-service configuration.
External NSS backends may require compatible host modules.

Install the fork from its Homebrew tap. If Homebrew already has stock tmux
installed, uninstall that formula first with `brew uninstall tmux`.

    brew install ZHDI-1/tmux/tmux

On Fedora, enable the Copr repository and install `tmux`.

    sudo dnf copr enable zhdi027/tmux-io256
    sudo dnf install tmux

If Fedora already has tmux installed, use `sudo dnf upgrade tmux`.

Launch it with `tmux`. To start a separate server, run `tmux -L io256 new-session`.
