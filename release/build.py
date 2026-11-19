#!/usr/bin/env python3
"""Build the release with locked third-party sources on Linux or macOS."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import platform
import shutil
import subprocess
import tarfile
import time

HERE = Path(__file__).resolve().parent
SYSTEM = platform.system()
DEPS = {
    name: dep for name, dep in json.loads((HERE / "dependencies.json").read_text()).items()
    if dep.get("platform", SYSTEM) == SYSTEM
}
FALLBACKS = (
    "xterm,xterm-256color,xterm-direct,screen,screen-256color,tmux,"
    "tmux-256color,linux,vt100,alacritty,alacritty-direct,kitty,xterm-kitty,"
    "foot,foot-direct,wezterm,ghostty"
)

def digest(path):
    return hashlib.file_digest(path.open("rb"), "sha256").hexdigest()

def dependencies(cache, offline):
    cache.mkdir(parents=True, exist_ok=True)
    for dep in DEPS.values():
        archive = cache / dep["archive"]
        if not archive.exists():
            if offline:
                raise FileNotFoundError(archive)
            partial = archive.with_suffix(".partial")
            subprocess.run(["curl", "--fail", "--location", "--retry", "3",
                            "--output", partial, dep["url"]], check=True)
            if digest(partial) != dep["sha256"]:
                raise ValueError(f"Invalid download checksum: {archive.name}")
            partial.replace(archive)
        if digest(archive) != dep["sha256"]:
            raise ValueError(f"Invalid checksum: {archive}")

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", required=True, type=Path)
    parser.add_argument("--source", type=Path)
    parser.add_argument("--work", type=Path)
    parser.add_argument("--offline", action="store_true")
    args = parser.parse_args()
    cache = args.cache.resolve()
    dependencies(cache, args.offline)
    if args.source is None:
        return
    source = args.source.resolve()
    work = args.work.resolve()
    work.mkdir()
    prefix = work / "prefix"
    linux = SYSTEM == "Linux"
    make = "make" if linux else "gmake"
    env = os.environ.copy()
    for key in ("CPATH", "C_INCLUDE_PATH", "LIBRARY_PATH", "PKG_CONFIG_PATH",
                "PKG_CONFIG_SYSROOT_DIR", "CFLAGS", "CPPFLAGS"):
        env.pop(key, None)
    env.update(CC="gcc" if linux else "clang", CFLAGS="-O2",
               PKG_CONFIG_LIBDIR=str(prefix / "lib/pkgconfig"))
    extra_link = env.pop("RELEASE_LDFLAGS", "")
    env.pop("LDFLAGS", None)
    if not linux:
        env["MACOSX_DEPLOYMENT_TARGET"] = "15.0"
    jobs = min(12, os.cpu_count() or 2)
    sources = {}
    for name, dep in DEPS.items():
        with tarfile.open(cache / dep["archive"]) as tar:
            top = tar.getmembers()[0].name.split("/")[0]
            tar.extractall(work, filter="data")
        sources[name] = work / top

    started = time.monotonic()
    with (work / "build.log").open("w", buffering=1) as log:
        def run(command, cwd):
            command = list(map(str, command))
            print("+", *command, file=log)
            subprocess.run(command, cwd=cwd, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)

        def make_install(directory):
            run([make, f"-j{jobs}"], directory)
            run([make, "install"], directory)

        def cmake(name, options):
            build = work / f"build-{name}"
            run(["cmake", "-S", sources[name], "-B", build,
                 "-DCMAKE_BUILD_TYPE=Release", f"-DCMAKE_C_COMPILER={env['CC']}",
                 f"-DCMAKE_INSTALL_PREFIX={prefix}", "-DCMAKE_INSTALL_LIBDIR=lib",
                 f"-DCMAKE_PREFIX_PATH={prefix}", "-DBUILD_SHARED_LIBS=OFF",
                 "-DCMAKE_POLICY_VERSION_MINIMUM=3.5", *options], work)
            run(["cmake", "--build", build, "--parallel", jobs], work)
            run(["cmake", "--install", build], work)

        event = sources["libevent"]
        run(["./configure", f"--prefix={prefix}", "--disable-shared",
             "--enable-static", "--disable-openssl", "--disable-samples",
             "--disable-libevent-regress"], event)
        make_install(event)
        cmake("utf8proc", ["-DUTF8PROC_ENABLE_TESTING=OFF"])
        cmake("zlib-ng", ["-DZLIB_COMPAT=ON", "-DZLIB_ENABLE_TESTS=OFF",
                         "-DWITH_NATIVE_INSTRUCTIONS=OFF", "-DWITH_OPTIM=OFF"])
        cmake("libpng", ["-DPNG_SHARED=OFF", "-DPNG_STATIC=ON", "-DPNG_TESTS=OFF",
                         "-DPNG_TOOLS=OFF", "-DPNG_HARDWARE_OPTIMIZATIONS=OFF",
                         f"-DZLIB_LIBRARY={prefix}/lib/libz.a",
                         f"-DZLIB_INCLUDE_DIR={prefix}/include"])
        ncurses = sources["ncurses"]
        run(["patch", "-p1", "-i", HERE / "ncurses-terminfo.patch"], ncurses)
        build = work / "build-ncurses"
        build.mkdir()
        terminfo = "/usr/share/terminfo"
        run([ncurses / "configure", f"--prefix={prefix}", f"--libdir={prefix}/lib",
             "--enable-widec", "--with-termlib", "--with-normal", "--without-shared",
             "--without-debug", "--without-progs", "--without-tests",
             "--without-manpages", "--without-cxx", "--without-cxx-binding",
             "--without-ada", "--disable-db-install",
             f"--with-default-terminfo-dir={terminfo}",
             f"--with-terminfo-dirs=/etc/terminfo:/lib/terminfo:{terminfo}",
             "--enable-pc-files", f"--with-pkg-config-libdir={prefix}/lib/pkgconfig",
             f"--with-fallbacks={FALLBACKS}", f"--with-build-cc={env['CC']}"], build)
        run([make, "-C", "ncurses", "-B", "fallback.c"], build)
        make_install(build)
        if not linux:
            jemalloc = sources["jemalloc"]
            run(["./configure", f"--prefix={prefix}", f"--libdir={prefix}/lib",
                 "--disable-shared", "--enable-static"], jemalloc)
            make_install(jemalloc)
            # Darwin's malloc-zone registration lives in a constructor-only
            # archive member. Keep it when linking the static library.
            extra_link += f" -Wl,-force_load,{prefix}/lib/libjemalloc.a"
        run(["sh", "autogen.sh"], source)
        platform_options = ["--enable-static"] if linux else ["--enable-jemalloc"]
        static_link = "-static " if linux else ""
        run(["./configure", "--prefix=/usr", "--disable-debug", "--enable-images",
             "--enable-utf8proc", "--disable-systemd", "--disable-utempter", *platform_options,
             "CFLAGS=-O2 -g -fno-omit-frame-pointer -pthread",
             f"CPPFLAGS=-I{prefix}/include -I{prefix}/include/ncursesw",
             f"LDFLAGS={static_link}-pthread -L{prefix}/lib {extra_link}", "YACC=bison -y"], source)
        run([make, f"-j{jobs}"], source)
    binary = source / "tmux"
    subprocess.run([binary, "-V"], check=True)
    if linux:
        elf = subprocess.check_output(["readelf", "-l", "-d", binary], text=True)
        if "INTERP" in elf or "(NEEDED)" in elf:
            raise ValueError("Linux binary depends on shared libraries at startup")
    else:
        symbols = subprocess.check_output(["nm", "-jU", binary], text=True).splitlines()
        if not any(symbol.endswith("zone_register") for symbol in symbols):
            raise ValueError("jemalloc's Darwin malloc-zone constructor was omitted")
        libs = subprocess.check_output(["otool", "-L", binary], text=True).splitlines()[1:]
        if not libs or any(not line.strip().startswith(("/usr/lib/", "/System/Library/"))
                           for line in libs):
            raise ValueError(f"macOS binary depends on non-system libraries: {libs}")
    print(f"Built {binary} in {time.monotonic() - started:.0f} seconds")

if __name__ == "__main__":
    main()
