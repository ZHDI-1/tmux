# Publishing tmux io256

The initial source pin is the tested alpha source. Its file hashes are recorded
in `alpha1-source.json`. Linux targets use static glibc. macOS targets use the
system runtime and link the pinned third-party libraries into the binary.

For each release, commit the source, set its full commit ID in `pin.json` and
increment the release number. Push the release branch and dispatch
**io256 release** from `release/io256`. The default dispatch only builds and
tests. Set `publish=true` to publish after all four targets pass. The release
contains per-platform tarballs, an offline source RPM, a Brew formula, checksums
and the source pin. A published tag is immutable in the workflow.

The [Homebrew tap](https://github.com/ZHDI-1/homebrew-tmux) follows the latest
published io256 release and checks its formula checksum. It syncs every hour
and supports a manual dispatch. It uses its own repository token.

To activate Fedora publication, add the Copr API config as the repository
Actions secret `COPR_CONFIG`. The workflow uses the configured username by
default. Set the Actions variable `COPR_OWNER` to use another owner or group
where the account has publication access. The job creates `tmux-io256` with
Fedora 44 and 45 chroots on first use and submits the published source RPM to Copr.
The RPM is built from the vendored, checksum-pinned sources with network
access disabled. Dispatch **io256 Copr publication** with an existing release tag to submit
an already published source RPM without rebuilding the four archives.

See [installation instructions](INSTALL.md).
