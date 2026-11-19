# tmux io256

The release pin in the fork identifies the source revision. The first release
carries the tested io256 alpha source, so `tmux -V` still reports
`next-3.9-alpha1-io256`. The Linux executables link glibc and the third-party
dependencies statically. The macOS executables use Apple's system libraries
and include the third-party dependencies, including jemalloc. The macOS deployment target is 15.

Start a separate server from an extracted archive:

    ./tmux -L io256 new-session

On Linux, if the system lacks a UTF-8 locale, point glibc at the bundled data:

    LOCPATH="$PWD/locale" ./tmux -L io256 new-session

The Linux binary uses the host's ordinary name-service configuration.
External NSS backends may require compatible host modules.

The Homebrew formula installs `tmux-io256` alongside stock tmux:

    brew install ZHDI-1/tmux/tmux-io256

After the Copr repository is populated, enable it and install `tmux-io256`:

    sudo dnf copr enable COPR_OWNER/tmux-io256
    sudo dnf install tmux-io256

For either package, start a new server with `tmux-io256 -L io256 new-session`.
