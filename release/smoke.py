"""Exercise the actual release executable with an isolated server."""

from pathlib import Path
import os
import subprocess
import sys
import tempfile
import time


def smoke(binary, directory, mode=None):
    socket = str(directory / "socket")
    config = directory / "tmux.conf"
    config.write_text("set -g default-shell /bin/sh\nset -g remain-on-exit on\n")
    env = {key: os.environ[key] for key in ("HOME", "USER", "LOGNAME")
           if key in os.environ}
    env.update(PATH="/usr/bin:/bin", SHELL="/bin/sh", TERM="xterm-256color",
               LC_ALL="en_US.UTF-8")
    if mode is not None:
        env["TMUX_IO_MODE"] = mode

    def tm(*args, check=True):
        process = subprocess.run([binary, "-vv", "-S", socket, "-f", config, *args],
                                 cwd=directory, env=env, text=True,
                                 capture_output=True, timeout=15)
        if check and process.returncode:
            raise RuntimeError(f"{args}: status {process.returncode}: {process.stderr.strip()}")
        return process.stdout

    try:
        tm("new-session", "-d", "-s", "release",
           "printf 'release smoke 中文\\n'; exec /bin/sleep 60")
        for _ in range(100):
            if "release smoke 中文" in tm("capture-pane", "-p"):
                if tm("display-message", "-p", "#{pane_dead}").strip() == "1":
                    raise AssertionError("The pane exited unexpectedly")
                break
            time.sleep(0.05)
        else:
            raise AssertionError("Pane output did not arrive")
        print(tm("display-message", "-p", "#{version}").strip())
    except Exception:
        for path in sorted(directory.glob("tmux-server-*.log")):
            print(f"{path.name} (last 100 lines)", file=sys.stderr)
            print("\n".join(path.read_text(errors="replace").splitlines()[-100:]),
                  file=sys.stderr)
        raise
    finally:
        tm("kill-server", check=False)


binary = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix="tmux-release-", dir="/tmp") as scratch:
    directory = Path(scratch)
    default = directory / "default"
    default.mkdir()
    try:
        smoke(binary, default)
    except Exception:
        if sys.platform == "darwin":
            fallback = directory / "synchronous"
            fallback.mkdir()
            print("Checking TMUX_IO_MODE=off after the default-mode failure", file=sys.stderr)
            try:
                smoke(binary, fallback, "off")
            except Exception as error:
                print(f"Synchronous fallback also failed: {error}", file=sys.stderr)
            else:
                print("Synchronous fallback passed", file=sys.stderr)
        raise
