#!/usr/bin/env python3
"""Build a redistributable runtime from the pinned JDK; retain all applicable notices.

The build host's jlink links the target JDK's modules (--jdk), so a cross build
(Linux aarch64 from x64) links the aarch64 runtime with the host JDK
(--host-jdk); both are the same pinned JDK release.
"""
import argparse
import os
import shutil
import subprocess
from pathlib import Path
from convert_apk import DEPS, java_path

# r2-java outputs/external_type_union.json: java.util.logging and sun.misc
# {Unsafe,Signal,SignalHandler}. java.xml supports framework resource parsers.
# java.desktop backs the framework's android.graphics Bitmap/BitmapFactory/Paint
# (java.awt.image, javax.imageio, java.awt.Font); the JVM runs it headless.
# No java.sql or java.net.http.
MODULES = "java.base,java.logging,java.xml,java.desktop,jdk.unsupported"


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("output", type=Path)
    parser.add_argument("--jdk", type=Path, default=DEPS / "jdk", help="target JDK whose jmods are linked")
    parser.add_argument("--host-jdk", type=Path, default=DEPS / "jdk", help="build-host JDK running jlink")
    args = parser.parse_args()
    jlink = java_path(args.host_jdk / "bin" / ("jlink.exe" if os.name == "nt" else "jlink"))
    strip = ["--strip-debug"]
    if os.name != "nt" and args.jdk.resolve() != args.host_jdk.resolve():
        # --strip-debug runs the host's objcopy on the target's native libraries;
        # LLVM's reads every architecture.
        objcopy = shutil.which("llvm-objcopy")
        if objcopy is None:
            raise SystemExit("make_jre: a cross-architecture JRE needs llvm-objcopy on PATH")
        strip = ["--strip-java-debug-attributes", "--strip-native-debug-symbols", "objcopy=" + objcopy]
    subprocess.run([jlink, "--module-path", str(args.jdk / "jmods"), "--add-modules", MODULES, *strip,
                    "--no-header-files", "--no-man-pages", "--output", str(args.output)], check=True)
    # jlink supplies legal/<module>; retain distribution-level notices as well.
    for name in ("NOTICE", "LICENSE", "ADDITIONAL_LICENSE_INFO", "ASSEMBLY_EXCEPTION"):
        source = args.jdk / name
        if source.exists():
            shutil.copy2(source, args.output / name)
    (args.output / "venture-modules.txt").write_text(MODULES + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
