"""Exercise the actual release executable with an isolated server."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time

binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="tmux-release-", dir="/tmp") as work:
    socket = str(Path(work) / "socket")
    env = os.environ.copy()
    for key in tuple(env):
        if key in {"TMUX", "TMUX_PANE"} or key.startswith("TMUX_IO_"):
            del env[key]
    env.update(TERM="xterm-256color", LC_ALL="en_US.UTF-8")

    def tm(*args, check=True):
        return subprocess.run([binary, "-S", socket, "-f", "/dev/null", *args],
                              env=env, check=check, text=True, capture_output=True,
                              timeout=15)

    try:
        tm("new-session", "-d", "-s", "release", "printf 'release smoke 中文\\n'; exec sleep 15")
        for _ in range(100):
            if "release smoke 中文" in tm("capture-pane", "-p").stdout:
                break
            time.sleep(0.05)
        else:
            raise AssertionError("Pane output did not arrive")
        print(tm("display-message", "-p", "#{version}").stdout.strip())
    finally:
        tm("kill-server", check=False)
