"""Create Homebrew metadata and checksums from a complete build matrix."""

import hashlib
import json
from pathlib import Path
import sys

directory = Path(sys.argv[1])
tag = sys.argv[2]
repo = sys.argv[3]
targets = ["linux-x86_64", "linux-arm64", "macos-x86_64", "macos-arm64"]
archives = {target: directory / f"tmux-io256-{tag}-{target}.tar.gz"
            for target in targets}

def digest(path):
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()

arm = archives["macos-arm64"]
intel = archives["macos-x86_64"]
base = f"https://github.com/{repo}/releases/download/{tag}"
formula = f'''class Tmux < Formula
  desc "Tmux terminal multiplexer with the io256 PTY reader"
  homepage "https://github.com/{repo}"
  version "{tag.removeprefix('v')}"
  license "ISC"
  depends_on macos: :sequoia

  on_arm do
    url "{base}/{arm.name}"
    sha256 "{digest(arm)}"
  end

  on_intel do
    url "{base}/{intel.name}"
    sha256 "{digest(intel)}"
  end

  def install
    bin.install "tmux"
    man1.install "tmux.1"
    pkgshare.install "licenses"
  end

  test do
    assert_match "tmux ", shell_output("#{{bin}}/tmux -V")
  end
end
'''
(destination := directory / "tmux.rb").write_text(formula)
srpm = list(directory.glob("*.src.rpm"))
if len(srpm) != 1:
    raise ValueError(f"Expected exactly one SRPM, found {srpm}")
assets = [*archives.values(), destination, *srpm, directory / "pin.json"]
(directory / "SHA256SUMS").write_text(
    "".join(f"{digest(path)}  {path.name}\n" for path in sorted(assets))
)
