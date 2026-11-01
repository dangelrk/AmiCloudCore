#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
"""Server-Paket eines Projekts fuer ein Release (make server-release):

    build/release/<Projekt>-Server-<version>.tar.gz
      <Projekt>-Server-<version>/
        INSTALL.md   install/server/README.md (@VERSION@ ersetzt) + der
                     Server-Abschnitt aus docs/INSTALL.md
        LICENSE
        server/      alle versionierten Dateien aus server/ (ohne config.php)
        ...          weitere Dateien nach SERVER_EXTRA (quelle[:ziel])

Aufruf (aus dem Projektverzeichnis):
    serverpack.py PROJEKT VERSION AUSGABEORDNER "UEBERSCHRIFT" [quelle[:ziel] ...]
UEBERSCHRIFT ist die Zeile in docs/INSTALL.md, mit der der Server-Abschnitt
beginnt (z. B. "## Server"); er endet vor der naechsten Ueberschrift
derselben Ebene.

Das Archiv gehoert root:root, Zeiten und Reihenfolge kommen aus den
Dateien, macOS-Metadaten (._*, xattr) kommen nicht hinein."""
import io
import os
import subprocess
import sys
import tarfile


def die(msg):
    print(f"serverpack: {msg}", file=sys.stderr)
    sys.exit(1)


def section(path, heading):
    lines = open(path, encoding="utf-8").read().splitlines(keepends=True)
    try:
        start = next(i for i, l in enumerate(lines) if l.rstrip("\n") == heading)
    except StopIteration:
        die(f"{path}: Ueberschrift {heading!r} nicht gefunden")
    level = len(heading) - len(heading.lstrip("#"))
    out, in_code = [lines[start]], False
    for l in lines[start + 1:]:
        if l.startswith("```"):
            in_code = not in_code
        if not in_code and l.startswith("#" * level + " "):
            break
        out.append(l)
    while out and out[-1].strip() in ("", "---"):
        out.pop()
    return "".join(out) + "\n"


def main():
    if len(sys.argv) < 5:
        die(__doc__.strip().splitlines()[0])
    project, version, outdir, heading = sys.argv[1:5]
    extras = sys.argv[5:]
    top = f"{project}-Server-{version}"

    files = subprocess.run(["git", "ls-files", "-z", "server"], capture_output=True,
                           check=True).stdout.decode().split("\0")
    members = []   # (archive name, source path)
    for f in sorted(x for x in files if x):
        if os.path.basename(f) == "config.php":
            continue
        members.append((f"{top}/{f}", f))
    if not members:
        die("server/ hat keine versionierten Dateien")
    for e in extras:
        src, _, dst = e.partition(":")
        if not os.path.isfile(src):
            die(f"{src} fehlt")
        members.append((f"{top}/{dst or src}", src))
    if os.path.isfile("LICENSE"):
        members.append((f"{top}/LICENSE", "LICENSE"))

    readme = "install/server/README.md"
    if not os.path.isfile(readme):
        die(f"{readme} fehlt")
    guide = open(readme, encoding="utf-8").read().replace("@VERSION@", version)
    guide = guide.rstrip("\n") + "\n\n" + section("docs/INSTALL.md", heading)

    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, f"{top}.tar.gz")

    def norm(ti):
        ti.uid = ti.gid = 0
        ti.uname = ti.gname = "root"
        ti.mode = 0o755 if ti.isdir() or ti.mode & 0o100 else 0o644
        return ti

    dirs = set()
    with tarfile.open(out, "w:gz", format=tarfile.PAX_FORMAT) as tar:
        def add_dirs(name, mtime):
            parts = name.split("/")[:-1]
            for i in range(1, len(parts) + 1):
                d = "/".join(parts[:i])
                if d not in dirs:
                    dirs.add(d)
                    ti = tarfile.TarInfo(d)
                    ti.type, ti.mtime = tarfile.DIRTYPE, mtime
                    tar.addfile(norm(ti))
        data = guide.encode("utf-8")
        ti = tarfile.TarInfo(f"{top}/INSTALL.md")
        ti.size, ti.mtime = len(data), int(os.path.getmtime(readme))
        add_dirs(ti.name, ti.mtime)
        tar.addfile(norm(ti), io.BytesIO(data))
        for name, src in members:
            ti = tar.gettarinfo(src, arcname=name)
            add_dirs(name, int(ti.mtime))
            with open(src, "rb") as fh:
                tar.addfile(norm(ti), fh)
    print(f"{out}: {len(members) + 1} Dateien, {os.path.getsize(out)} Bytes")


if __name__ == "__main__":
    main()
