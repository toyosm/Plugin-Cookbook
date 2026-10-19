#!/usr/bin/env python3
# Plugin Cookbook — packages Plugin Studio as an app people can download and double-click.
# SPDX-License-Identifier: MIT
"""
    python3 tools/package/package_studio.py macos   [--out dist]   -> dist/Plugin-Studio-macOS.zip
    python3 tools/package/package_studio.py windows [--out dist]   -> dist/Plugin-Studio-Windows.zip

Each package carries a portable Python (python-build-standalone), CMake (from its PyPI wheel) and a snapshot
of this repository (tracked files only, without your own recipes). On first run the launcher copies the
snapshot to ~/Plugin Cookbook (macOS) or Documents/Plugin Cookbook (Windows), offers to install the C++
compiler and Claude Code if they are missing, and opens the Studio. The macOS package must be built on a
Mac (it uses osacompile, iconutil and ditto); the Windows package can be built anywhere.
"""

from __future__ import annotations

import argparse
import hashlib
import math
import os
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zipfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

# Portable CPython builds; pinned, and checked against the release's SHA256SUMS.
PBS_RELEASE = "20260924"
PBS_VERSION = "3.12.14"
PBS_URL = "https://github.com/astral-sh/python-build-standalone/releases/download/" + PBS_RELEASE + "/"
PBS_ASSETS = {
    "macos-arm64": f"cpython-{PBS_VERSION}+{PBS_RELEASE}-aarch64-apple-darwin-install_only.tar.gz",
    "macos-x86_64": f"cpython-{PBS_VERSION}+{PBS_RELEASE}-x86_64-apple-darwin-install_only.tar.gz",
    "windows-x64": f"cpython-{PBS_VERSION}+{PBS_RELEASE}-x86_64-pc-windows-msvc-install_only.tar.gz",
}
CMAKE_VERSION = "3.31.6"
CMAKE_PLATFORM = {"macos": "macosx_10_10_universal2", "windows": "win_amd64"}

# Top-level folders the launcher keeps in sync with the app (your plugins/ and build/ are never touched).
MANAGED_DIRS = ["framework", "tools", "dpf", "dpf-widgets", "examples", "prompts", "docs"]


def cache_dir() -> Path:
    d = ROOT / "build" / "package-cache"
    d.mkdir(parents=True, exist_ok=True)
    return d


def stage_dir(name: str) -> Path:
    """A fresh staging folder outside the repository: cloud-synced folders (iCloud Documents) keep adding
    Finder attributes to bundles, which codesign rejects."""
    d = Path(tempfile.gettempdir()) / "plugin-studio-package" / name
    shutil.rmtree(d, ignore_errors=True)
    d.mkdir(parents=True)
    return d


def download(url: str, dest: Path) -> Path:
    if not dest.exists():
        print("downloading", url)
        tmp = dest.with_suffix(dest.suffix + ".part")
        if shutil.which("curl"):  # uses the system's certificates (python.org builds may have none set up)
            subprocess.run(["curl", "-fsSL", "-o", str(tmp), url], check=True)
        else:
            with urllib.request.urlopen(url) as r, open(tmp, "wb") as f:
                shutil.copyfileobj(r, f)
        tmp.rename(dest)
    return dest


def portable_python(key: str, dest: Path) -> None:
    """Extracts python-build-standalone into dest (dest/bin/python3 or dest/python.exe)."""
    name = PBS_ASSETS[key]
    sums = download(PBS_URL + "SHA256SUMS", cache_dir() / f"pbs-{PBS_RELEASE}-SHA256SUMS")
    expected = next((line.split()[0] for line in sums.read_text().splitlines() if line.endswith(" " + name)), None)
    archive = download(PBS_URL + name, cache_dir() / name)
    actual = hashlib.sha256(archive.read_bytes()).hexdigest()
    if expected != actual:
        archive.unlink()
        sys.exit(f"checksum mismatch for {name}")
    with tarfile.open(archive) as t:
        t.extractall(dest.parent / "_extract", filter="tar")
    shutil.move(str(dest.parent / "_extract" / "python"), str(dest))
    shutil.rmtree(dest.parent / "_extract")
    # the Studio needs only the standard library: drop the test suite, IDLE, Tk and friends (~half the size)
    lib = next(dest.glob("lib/python3*"), None) or dest / "Lib"
    for name in ("test", "idlelib", "tkinter", "turtledemo", "ensurepip", "lib2to3", "pydoc_data"):
        shutil.rmtree(lib / name, ignore_errors=True)
    for pattern in ("lib/tcl*", "lib/tk*", "lib/itcl*", "lib/thread*", "tcl", "share", "include"):
        for p in dest.glob(pattern):
            shutil.rmtree(p, ignore_errors=True) if p.is_dir() else p.unlink()


def portable_cmake(platform_key: str, dest: Path) -> Path:
    """Installs the CMake wheel for the target platform into dest; returns the folder holding cmake(.exe)."""
    subprocess.run([sys.executable, "-m", "pip", "install", "--quiet", "--no-deps", "--only-binary=:all:",
                    "--platform", CMAKE_PLATFORM[platform_key], "--target", str(dest), f"cmake=={CMAKE_VERSION}"],
                   check=True)
    exe = "cmake.exe" if platform_key == "windows" else "cmake"
    for p in [p for p in dest.rglob("*") if p.is_dir() and p.name in ("doc", "man")]:  # not needed to build
        shutil.rmtree(p, ignore_errors=True)
    found = [p for p in dest.rglob(exe) if p.is_file() and p.parent.name == "bin" and "data" in p.parts]
    if not found:
        sys.exit("cmake binary not found in the wheel")
    for tool in ("ccmake", "cpack", "ctest", "cmake-gui"):  # only cmake itself is used
        for p in found[0].parent.glob(tool + "*"):
            p.unlink()
    return found[0].parent


def snapshot(dest: Path) -> None:
    """Tracked files of the repository and its submodules, without your own recipes."""
    files = subprocess.run(["git", "ls-files", "--recurse-submodules", "-z"], cwd=ROOT, capture_output=True,
                           check=True).stdout.decode().split("\0")
    for rel in files:
        if not rel or (rel.startswith("plugins/") and rel != "plugins/README.md") or rel.startswith(".github/"):
            continue
        src = ROOT / rel
        if src.is_file():
            target = dest / rel
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(src, target)
    (dest / "plugins").mkdir(exist_ok=True)
    version = subprocess.run(["git", "describe", "--tags", "--always", "--dirty"], cwd=ROOT, capture_output=True,
                             text=True).stdout.strip() or "dev"
    (dest / "VERSION").write_text(version + "\n")


# ---------------------------------------------------------------------------------------------------------------------
# Icon: the "fluted" knob, drawn in plain Python


def write_png(path: Path, w: int, h: int, rgba: bytearray) -> None:
    raw = bytearray()
    for y in range(h):
        raw.append(0)
        raw += rgba[y * w * 4:(y + 1) * w * 4]

    def chunk(kind: bytes, data: bytes) -> bytes:
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    path.write_bytes(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 6, 0, 0, 0))
                     + chunk(b"IDAT", zlib.compress(bytes(raw), 9)) + chunk(b"IEND", b""))


def icon_png(size: int) -> bytearray:
    """A rounded orange tile with a black seven-lobed knob and a pale green pointer."""
    out = bytearray(size * size * 4)
    ss = 3 if size <= 128 else 2  # supersampling
    R = size * 0.30
    cx = cy = size / 2
    corner = size * 0.22
    for y in range(size):
        for x in range(size):
            acc = [0.0, 0.0, 0.0, 0.0]
            for sy in range(ss):
                for sx in range(ss):
                    px, py = x + (sx + 0.5) / ss, y + (sy + 0.5) / ss
                    # tile with rounded corners
                    qx = max(abs(px - cx) - (size / 2 - size * 0.06 - corner), 0)
                    qy = max(abs(py - cy) - (size / 2 - size * 0.06 - corner), 0)
                    if math.hypot(qx, qy) > corner:
                        continue
                    t = py / size
                    col = [0.95 - 0.12 * t, 0.42 - 0.12 * t, 0.11 - 0.05 * t]
                    dx, dy = px - cx, py - cy
                    r = math.hypot(dx, dy)
                    a = math.atan2(dy, dx) + math.pi / 2  # 0 = straight up
                    lobe = R * (0.84 + 0.07 * math.cos(7 * a)) / 0.91
                    if math.hypot(dx - R * 0.03, dy - R * 0.07) < R * 1.02:
                        col = [c * 0.75 for c in col]  # shadow
                    if r < R * 0.98:
                        col = [0.06, 0.06, 0.065]
                    if r < lobe:
                        col = [0.085, 0.085, 0.09]
                        if abs(r - lobe) < R * 0.035:
                            col = [0.62, 0.62, 0.62]
                    if r < R * 0.62 and math.hypot(dx + R * 0.2, dy + R * 0.25) < R * 0.3:
                        col = [c + 0.04 for c in col]
                    if abs(dx) < R * 0.07 and -R * 0.86 < dy < -R * 0.1:
                        col = [0.81, 0.90, 0.75]
                    acc = [acc[0] + col[0], acc[1] + col[1], acc[2] + col[2], acc[3] + 1]
            n = ss * ss
            i = (y * size + x) * 4
            alpha = acc[3] / n
            if alpha > 0:
                out[i:i + 4] = bytes([int(min(1, acc[k] / acc[3]) * 255) for k in range(3)] + [int(alpha * 255)])
    return out


def make_icns(dest: Path, work: Path) -> None:
    iconset = work / "AppIcon.iconset"
    iconset.mkdir(parents=True, exist_ok=True)
    for base in (16, 32, 128, 256, 512):
        for scale in (1, 2):
            px = base * scale
            name = f"icon_{base}x{base}{'@2x' if scale == 2 else ''}.png"
            write_png(iconset / name, px, px, icon_png(px))
    subprocess.run(["iconutil", "-c", "icns", str(iconset), "-o", str(dest)], check=True)


# ---------------------------------------------------------------------------------------------------------------------
# Packages


def package_macos(out: Path) -> Path:
    work = stage_dir("macos")
    app = work / "Plugin Studio.app"

    # A stay-open AppleScript applet: a real app with a Dock icon and a Quit menu that stops the server.
    subprocess.run(["osacompile", "-s", "-o", str(app), str(HERE / "mac" / "applet.applescript")], check=True)
    res = app / "Contents" / "Resources"
    plist = app / "Contents" / "Info.plist"
    for key, value in (("CFBundleIdentifier", "org.plugincookbook.studio"), ("CFBundleName", "Plugin Studio"),
                       ("CFBundleDisplayName", "Plugin Studio"), ("CFBundleShortVersionString", "1.0"),
                       ("NSAppleEventsUsageDescription", "Plugin Studio opens Terminal to install Claude Code.")):
        subprocess.run(["plutil", "-replace", key, "-string", value, str(plist)], check=True)

    shutil.copy2(HERE / "mac" / "launch.sh", res / "launch.sh")
    os.chmod(res / "launch.sh", 0o755)
    snapshot(res / "cookbook")
    portable_python("macos-arm64", res / "python-arm64")
    portable_python("macos-x86_64", res / "python-x86_64")
    cmake_bin = portable_cmake("macos", res / "cmake-wheel")
    (res / "cmake-bin.txt").write_text(str(cmake_bin.relative_to(res)) + "\n")
    make_icns(res / "applet.icns", work)

    subprocess.run(["xattr", "-cr", str(app)], check=True)
    subprocess.run(["codesign", "--force", "--deep", "--sign", "-", str(app)], check=True)
    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / "Plugin-Studio-macOS.zip"
    zip_path.unlink(missing_ok=True)
    subprocess.run(["ditto", "-c", "-k", "--keepParent", str(app), str(zip_path)], check=True)
    return zip_path


def package_windows(out: Path) -> Path:
    work = stage_dir("windows")
    top = work / "Plugin Studio"
    (top / "setup").mkdir(parents=True)
    # Windows line endings for the files people and cmd.exe read
    for src, dest in (("Plugin Studio.cmd", top / "Plugin Studio.cmd"), ("launch.ps1", top / "setup" / "launch.ps1"),
                      ("README.txt", top / "README.txt")):
        text = (HERE / "windows" / src).read_text(encoding="utf-8").replace("\r\n", "\n")
        dest.write_bytes(text.replace("\n", "\r\n").encode("utf-8"))
    snapshot(top / "cookbook")
    portable_python("windows-x64", top / "python")
    cmake_bin = portable_cmake("windows", top / "cmake-wheel")
    (top / "setup" / "cmake-bin.txt").write_text(str(cmake_bin.relative_to(top)).replace("/", "\\") + "\n")

    out.mkdir(parents=True, exist_ok=True)
    zip_path = out / "Plugin-Studio-Windows.zip"
    zip_path.unlink(missing_ok=True)
    with zipfile.ZipFile(zip_path, "w", zipfile.ZIP_DEFLATED) as z:
        for f in sorted(top.rglob("*")):
            if f.is_file():
                z.write(f, f.relative_to(work).as_posix())
    return zip_path


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("platform", choices=["macos", "windows"])
    ap.add_argument("--out", default=str(ROOT / "dist"))
    args = ap.parse_args()
    out = Path(args.out)
    path = package_macos(out) if args.platform == "macos" else package_windows(out)
    print(f"wrote {path} ({path.stat().st_size / 1e6:.0f} MB)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
