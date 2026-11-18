#!/usr/bin/env python3
"""Assemble the aarch64 Linux release sysroot (Ubuntu 22.04 arm64, glibc 2.35).

  python3 tools/linux-baseline/aarch64_sysroot.py [--out DIR]

The arm64 counterpart of tools/linux-baseline/Containerfile, for cross builds
(cmake/toolchains/linux-aarch64.cmake): the same glibc baseline, development
packages and LLVM libc++ (apt.llvm.org), unpacked without running any arm64
code, so no binfmt/QEMU registration or root is needed. Binaries linked against
it run on glibc >= 2.35 aarch64 distributions (SteamOS on the Steam Frame).
Default output: .deps/sysroot-aarch64. Re-running rebuilds it from scratch.

Every suite's InRelease is verified with gpgv against the pinned archive key
fingerprint, each Packages index against InRelease's SHA256, and every .deb
(fetched or cached) against its index entry, as bootstrap.py pins its downloads.
"""
from __future__ import annotations

import argparse
import base64
import gzip
import hashlib
import io
import lzma
import os
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
LLVM = "22"
UBUNTU = "https://ports.ubuntu.com/ubuntu-ports"
LLVM_APT = "https://apt.llvm.org/jammy"
# Ubuntu Archive Automatic Signing Key (2018) and apt.llvm.org's (Sylvestre Ledru).
UBUNTU_KEY = ("https://keyserver.ubuntu.com/pks/lookup?op=get&search=0xF6ECB3762474EDA9D21B7022871920D1991BC93C",
              "F6ECB3762474EDA9D21B7022871920D1991BC93C")
LLVM_KEY = ("https://apt.llvm.org/llvm-snapshot.gpg.key", "6084F3CF814B57C1CF12EFD515CF4D18AF4F7421")
# (base, suite, key, indexes); searched last to first: a later suite's version of a package wins.
SUITES = [
    (UBUNTU, suite, UBUNTU_KEY, [f"{component}/binary-arm64/Packages.xz" for component in ("main", "universe")])
    for suite in ("jammy", "jammy-updates", "jammy-security")
] + [(LLVM_APT, f"llvm-toolchain-jammy-{LLVM}", LLVM_KEY, ["main/binary-arm64/Packages.gz"])]

# Containerfile's development packages, for arm64.
PACKAGES = [
    "libc6-dev", "libgcc-11-dev", "linux-libc-dev",
    f"libc++-{LLVM}-dev", f"libc++abi-{LLVM}-dev", f"libunwind-{LLVM}-dev",
    "libssl-dev", "libsecret-1-dev",
    "libwayland-dev", "libxkbcommon-dev", "libegl-dev", "libgles-dev", "libdrm-dev", "libgbm-dev",
    "libpipewire-0.3-dev", "libpulse-dev", "libasound2-dev", "libdbus-1-dev", "libudev-dev", "libusb-1.0-0-dev",
    "libvulkan-dev",
]
# Tools and interpreters pulled in by -dev packages; a sysroot needs only headers and libraries.
SKIP_PREFIXES = ("perl", "python3", "libpython3", "debconf", "dpkg", "systemd", "init-system-helpers",
                 "sensible-utils", "install-info", "ucf", "adduser", "passwd", "lsb-base", "tzdata",
                 "gcc-12-base", "gcc-11-base", "cpp", "gcc-11", "bash", "coreutils", "readline-common",
                 "libglib2.0-bin", "libglib2.0-dev-bin", "dbus", "pkg-config", "pkgconf", "mime-support",
                 "media-types", "shared-mime-info", "xdg-user-dirs", "libgcc-s1-dbg", "libllvm",
                 "llvm-", "clang-", "lld-", "wayland-protocols")


def fetch(url: str) -> bytes:
    with urllib.request.urlopen(url) as response:
        return response.read()


def sha256(blob: bytes) -> str:
    return hashlib.sha256(blob).hexdigest()


def dearmor(armored: str) -> bytes:
    lines = armored.strip().splitlines()
    body = lines[lines.index("") + 1:]
    return base64.b64decode("".join(line for line in body if not line.startswith(("=", "-----"))))


def verified_release(base: str, suite: str, key: tuple[str, str], keys: Path) -> dict[str, str]:
    """SHA256 of each index file listed by the suite's InRelease, after checking its signature."""
    keyring = keys / (key[1] + ".gpg")
    if not keyring.exists():
        keyring.write_bytes(dearmor(fetch(key[0]).decode()))
    release = keys / (suite + ".InRelease")
    release.write_bytes(fetch(f"{base}/dists/{suite}/InRelease"))
    status = subprocess.run(["gpgv", "--status-fd", "1", "--keyring", str(keyring), str(release)],
                            capture_output=True, text=True).stdout
    if f"[GNUPG:] VALIDSIG {key[1]} " not in status and not any(
            line.startswith("[GNUPG:] VALIDSIG ") and line.split()[-1] == key[1] for line in status.splitlines()):
        raise SystemExit(f"aarch64_sysroot: {base}/dists/{suite}/InRelease is not signed by {key[1]}")
    hashes: dict[str, str] = {}
    in_sha256 = False
    # Only the signed part between the PGP armor lines counts.
    signed = release.read_text().split("-----BEGIN PGP SIGNATURE-----")[0]
    for line in signed.splitlines():
        if not line.startswith(" "):
            in_sha256 = line.startswith("SHA256:")
        elif in_sha256:
            digest, _, name = line.split()
            hashes[name] = digest
    return hashes


def parse_index(text: str, base: str, packages: dict[str, dict], provides: dict[str, str]) -> None:
    for stanza in text.split("\n\n"):
        fields: dict[str, str] = {}
        key = None
        for line in stanza.splitlines():
            if line.startswith((" ", "\t")) and key:
                fields[key] += "\n" + line
            elif ":" in line:
                key, value = line.split(":", 1)
                fields[key] = value.strip()
        if "Package" not in fields or fields.get("Architecture") not in ("arm64", "all"):
            continue
        fields["_base"] = base
        packages[fields["Package"]] = fields
        for virtual in fields.get("Provides", "").split(","):
            name = virtual.strip().split(" ")[0]
            if name:
                provides.setdefault(name, fields["Package"])


def resolve(roots: list[str], packages: dict[str, dict], provides: dict[str, str]) -> list[str]:
    chosen: list[str] = []
    seen: set[str] = set()
    pending = list(roots)
    while pending:
        name = pending.pop()
        if name in seen:
            continue
        seen.add(name)
        if name not in packages:
            raise SystemExit(f"aarch64_sysroot: no arm64 package {name}")
        chosen.append(name)
        for field in ("Pre-Depends", "Depends"):
            for group in packages[name].get(field, "").split(","):
                alternatives = [a.strip().split(" ")[0].split(":")[0] for a in group.split("|") if a.strip()]
                picked = None
                for alternative in alternatives:
                    if alternative in seen or alternative in packages:
                        picked = alternative
                        break
                    if alternative in provides:
                        picked = provides[alternative]
                        break
                if picked and not picked.startswith(SKIP_PREFIXES):
                    pending.append(picked)
    return sorted(chosen)


def deb_data(blob: bytes) -> tarfile.TarFile:
    if not blob.startswith(b"!<arch>\n"):
        raise ValueError("not a .deb archive")
    offset = 8
    while offset < len(blob):
        header = blob[offset:offset + 60]
        name = header[:16].decode().strip().rstrip("/")
        size = int(header[48:58].decode().strip())
        body = blob[offset + 60:offset + 60 + size]
        offset += 60 + size + (size & 1)
        if not name.startswith("data.tar"):
            continue
        if name.endswith(".zst"):
            try:
                from compression import zstd  # Python >= 3.14
                body = zstd.decompress(body)
            except ImportError:
                body = subprocess.run(["zstd", "-dc"], input=body, stdout=subprocess.PIPE, check=True).stdout
        elif name.endswith(".xz"):
            body = lzma.decompress(body)
        elif name.endswith(".gz"):
            body = gzip.decompress(body)
        return tarfile.open(fileobj=io.BytesIO(body))
    raise ValueError("no data.tar member")


MERGED = ("bin", "sbin", "lib")


def merged_path(name: str) -> str:
    name = name.removeprefix("./")
    first, _, rest = name.partition("/")
    return f"usr/{first}/{rest}" if first in MERGED and rest else name


def skipped(path: str, member: tarfile.TarInfo) -> bool:
    # Documentation is dropped except each package's copyright (and doc directories
    # linked to another package's): tools/package_release.py ships the notices of
    # the libraries linked statically from the sysroot.
    parts = path.split("/")
    if parts[:3] == ["usr", "share", "doc"]:
        return not ((len(parts) == 5 and parts[4] == "copyright") or (len(parts) == 4 and member.issym()))
    return path in ("usr/bin", "usr/sbin", "usr/lib") or path.startswith("usr/share/man")


def unpack(blob: bytes, out: Path) -> None:
    root = out.resolve()
    with deb_data(blob) as archive:
        for member in archive.getmembers():
            path = merged_path(member.name)
            if path in ("", ".") or skipped(path, member):
                continue
            if path.startswith("/") or ".." in Path(path).parts:
                raise SystemExit(f"aarch64_sysroot: unsafe member path {member.name}")
            target = out / path
            # A path through an earlier symlink must still land inside the sysroot.
            if not Path(os.path.realpath(target.parent)).is_relative_to(root):
                raise SystemExit(f"aarch64_sysroot: member {member.name} escapes the sysroot")
            if member.isdir():
                target.mkdir(parents=True, exist_ok=True)
            elif member.issym() or member.islnk():
                link = member.linkname
                if member.islnk():
                    link = "/" + merged_path(link)
                if link.startswith("/"):
                    link = os.path.relpath(out / merged_path(link.lstrip("/")), target.parent)
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink() or target.exists():
                    target.unlink()
                target.symlink_to(link)
            elif member.isfile():
                target.parent.mkdir(parents=True, exist_ok=True)
                if target.is_symlink():
                    target.unlink()
                with archive.extractfile(member) as source, open(target, "wb") as sink:
                    shutil.copyfileobj(source, sink)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--out", type=Path, default=ROOT / ".deps" / "sysroot-aarch64")
    parser.add_argument("--cache", type=Path, default=ROOT / ".deps" / "fetch" / "sysroot-aarch64-debs")
    args = parser.parse_args()
    packages: dict[str, dict] = {}
    provides: dict[str, str] = {}
    if not shutil.which("gpgv"):
        raise SystemExit("aarch64_sysroot: gpgv is required to verify the package archives")
    with tempfile.TemporaryDirectory(prefix="venture-sysroot-keys-") as keys:
        for base, suite, key, indexes in SUITES:
            hashes = verified_release(base, suite, key, Path(keys))
            for index in indexes:
                print(f"index {base}/dists/{suite}/{index}", flush=True)
                blob = fetch(f"{base}/dists/{suite}/{index}")
                if hashes.get(index) != sha256(blob):
                    raise SystemExit(f"aarch64_sysroot: {suite}/{index} does not match its signed InRelease")
                text = (lzma.decompress(blob) if index.endswith(".xz") else gzip.decompress(blob)).decode()
                parse_index(text, base, packages, provides)
    selected = resolve(PACKAGES, packages, provides)
    if args.out.exists():
        shutil.rmtree(args.out)
    args.out.mkdir(parents=True)
    for directory in MERGED:
        (args.out / "usr" / directory).mkdir(parents=True, exist_ok=True)
        (args.out / directory).symlink_to(f"usr/{directory}")
    args.cache.mkdir(parents=True, exist_ok=True)
    for name in selected:
        fields = packages[name]
        cached = args.cache / Path(fields["Filename"]).name
        blob = cached.read_bytes() if cached.exists() else b""
        if sha256(blob) != fields["SHA256"]:
            print(f"fetch {name} {fields['Version']}", flush=True)
            blob = fetch(f"{fields['_base']}/{fields['Filename']}")
            if sha256(blob) != fields["SHA256"]:
                raise SystemExit(f"aarch64_sysroot: {fields['Filename']} does not match its signed index")
            cached.write_bytes(blob)
        unpack(blob, args.out)
    # Chromium's build (tools/angle/build_angle.py --target-cpu arm64) reads only usr/lib/pkgconfig.
    pkgconfig = args.out / "usr/lib/pkgconfig"
    pkgconfig.mkdir(parents=True, exist_ok=True)
    for pc in (args.out / "usr/lib/aarch64-linux-gnu/pkgconfig").glob("*.pc"):
        if not (pkgconfig / pc.name).exists():
            (pkgconfig / pc.name).symlink_to(f"../aarch64-linux-gnu/pkgconfig/{pc.name}")
    (args.out / "PACKAGES").write_text("".join(f"{n} {packages[n]['Version']}\n" for n in selected))
    print(f"aarch64 sysroot: {len(selected)} packages in {args.out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
