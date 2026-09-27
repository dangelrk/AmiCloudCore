#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
"""
gen-build-amiga.py -- generate the native AmigaOS build script "Build-Amiga"
for a project (AmiCloud, AmiOwnCloud, AmiVCS) from its Makefile.amiga-cross
files.

Why a generated AmigaDOS script instead of a native Makefile: vbcc's `vc`
runs fine on a real Amiga, but a Makefile full of Unix-relative paths
("../../core/...") does not -- AmigaDOS spells the parent directory "/",
not "..". Build-Amiga therefore calls vc once per source file with paths
relative to the project root (no parent references at all), then links
from inside the object drawer with short object names, so every line stays
well below the 512-character AmigaDOS Shell line limit.

The source of truth stays the Makefile.amiga-cross files (they are what the
host cross-build uses and what CI-style verification runs against): this
tool asks `make -n -B` which vc commands they would run and translates
exactly those.

Usage (from the project root, i.e. the directory containing core/):
    python3 core/tools/gen-build-amiga.py            # (re)write Build-Amiga
    python3 core/tools/gen-build-amiga.py --verify   # also build it on the host

--verify translates the generated script back into a POSIX sh script and
runs it with the host cross vbcc ($VBCC, see docs/BUILD.md), so a broken
source list or flag shows up before anyone runs it on a real Amiga.
Set VC_CONFIG=/path/to/aos68k if your toolchain's config/aos68k contains
absolute paths that do not match $VBCC (see docs/BUILD.md).
"""
import argparse, os, re, shlex, subprocess, sys, tempfile

V = "/VBCCROOT"  # placeholder for $(VBCC) in the dry-run output

# Host SDK include dir -> AmigaDOS script variable (set at the script's top)
SDK_VARS = {
    f"{V}/sdk/AmiSSL-v5/Developer/include": "$AMISSL_INC",
    f"{V}/sdk/Roadshow/netinclude": "$NET_INC",
    f"{V}/sdk/MUI_3.8/Developer/C/Include": "$MUI_INC",
}
# Provided by the native vbcc configuration (vincludeos3: etc.) on the Amiga
NATIVE_IMPLICIT = {
    f"{V}/sdk/NDK_3.2/Include_H",
    f"{V}/targets/m68k-amigaos/include",
}
# Object names are shortened ("core_parson.o" -> "c_parson.o") to keep the
# link line short.
SHORT_PREFIX = {"core": "c_", "http": "h_", "fs": "f_", "sync": "s_",
                "vcs": "v_", "gui": "g_", "guilib": "g_"}
MAX_LINE = 480  # AmigaDOS Shell limit is 512 incl. variable expansion


def dry_run(proj, reldir):
    out = subprocess.run(["make", "-n", "-B", "-f", "Makefile.amiga-cross",
                          f"VBCC={V}"], cwd=os.path.join(proj, reldir),
                         capture_output=True, text=True, check=True).stdout
    out = re.sub(r"\\\n", " ", out)
    return [l.strip() for l in out.splitlines()
            if l.strip().startswith(f"{V}/bin/vc")]


def parse(proj, reldir):
    """-> ({obj: (src_rel_to_root, [flags])}, [(binary, [objs], [ldflags])])"""
    compiles, links = {}, []
    for line in dry_run(proj, reldir):
        tok = shlex.split(line)[1:]
        if "-c" in tok:
            src = tok[tok.index("-c") + 1]
            obj = tok[tok.index("-o") + 1]
            flags = [t for t in tok if t not in ("-c", src, "-o", obj)]
            compiles[obj] = (os.path.normpath(os.path.join(reldir, src)), flags)
        else:
            out = tok[tok.index("-o") + 1]
            rest = [t for t in tok if t not in (out, "-o")]
            objs = [t for t in rest if t.endswith(".o")]
            ld = [t for t in rest[1:] if not t.endswith(".o")
                  and not t.startswith("-L") and not t.startswith("+")]
            links.append((os.path.basename(out), objs, ld))
    return compiles, links


def amiga_flags(flags, reldir):
    res = []
    for f in flags:
        if f.startswith("+"):
            continue
        if f.startswith("-I"):
            p = f[2:]
            if p in NATIVE_IMPLICIT:
                continue
            res.append("-I" + SDK_VARS.get(p, os.path.normpath(os.path.join(reldir, p))))
        else:
            res.append(f)
    return res


def short_obj(name):
    pre, _, rest = name.partition("_")
    return SHORT_PREFIX[pre] + rest if rest and pre in SHORT_PREFIX else name


def components(proj):
    res = []
    for top in ("apps", "handler"):
        base = os.path.join(proj, top)
        if os.path.isdir(base):
            res += [f"{top}/{d}" for d in sorted(os.listdir(base))
                    if os.path.exists(os.path.join(base, d, "Makefile.amiga-cross"))]
    return res


def generate(proj, name):
    body, host, uses_mui, warn = [], [], False, []
    for reldir in components(proj):
        compiles, links = parse(proj, reldir)
        for prog, objs, ld in links:
            odir = f"obj/amiga/{prog}"
            depth = odir.count("/") + 1
            body += ["", f"; ---- {prog}  ({reldir})", f'Echo "*N==> {prog}"',
                     f"If NOT EXISTS {odir}", f"  MakeDir {odir}", "EndIf"]
            host.append(f'echo "==> {prog}"; mkdir -p {odir}')
            names = []
            for o in objs:
                src, flags = compiles[o]
                af = amiga_flags(flags, reldir)
                uses_mui |= "-I$MUI_INC" in af
                on = short_obj(os.path.basename(o))
                names.append(on)
                line = f"vc +aos68k {' '.join(af)} -c {src} -o {odir}/{on}"
                if len(line) > MAX_LINE - 60:
                    warn.append(f"{prog}: compile line for {src} is {len(line)} chars")
                body.append(line)
                host.append(line.replace("vc +aos68k", '"$VC" +"$CFG"')
                            + ' -I"$NDK_INC" -I"$RT_INC"')
            link = (f"vc +aos68k -o {'/' * depth}bin/amiga/{prog} "
                    f"{' '.join(names)} {' '.join(ld)}").rstrip()
            if len(link) > MAX_LINE:
                warn.append(f"{prog}: link line is {len(link)} chars")
            body += [f"CD {odir}", link, f"CD {'/' * depth}"]
            host.append(f'( cd {odir} && "$VC" +"$CFG" -o {"../" * depth}bin/amiga/{prog} '
                        f'{" ".join(names)} -L"$RT_LIB" {" ".join(ld)} )')

    head = [
        f"; Build-Amiga -- natives Bauen von {name} auf AmigaOS 3.x mit vbcc",
        "; SPDX-License-Identifier: GPL-3.0-or-later",
        ";",
        "; Aufruf im Projekt-Wurzelverzeichnis (dort, wo core/ liegt):",
        ";     Execute Build-Amiga",
        ";",
        "; Ergebnis: bin/amiga/<Programm>, Objektdateien in obj/amiga/<Programm>/.",
        ";",
        "; Voraussetzungen: vbcc mit Target m68k-amigaos (vc im Pfad, NDK-Includes",
        "; ueber die vbcc-Konfiguration), AmiSSL-5-SDK, Roadshow-SDK (netinclude)"
        + (", MUI-3.8-SDK." if uses_mui else "."),
        "; Die Pfade unten an das eigene System anpassen -- Details in docs/BUILD.md.",
        ";",
        "; ERZEUGTE DATEI -- nicht von Hand bearbeiten. Quelle sind die",
        "; Makefile.amiga-cross-Dateien; neu erzeugen mit",
        ";     python3 core/tools/gen-build-amiga.py --verify",
        "",
        'Set AMISSL_INC "AmiSSL:Developer/include"',
        'Set NET_INC    "NetInclude:"',
    ]
    if uses_mui:
        head.append('Set MUI_INC    "MUI:Developer/C/Include"')
    head += ["", "FailAt 10"]
    for d in ("bin", "bin/amiga", "obj", "obj/amiga"):
        head += [f"If NOT EXISTS {d}", f"  MakeDir {d}", "EndIf"]
    tail = ["", 'Echo "*NFertig. Die Programme liegen in bin/amiga/."', ""]
    script = "\n".join(head + body + tail)

    host_sh = "\n".join([
        "#!/bin/sh", "set -e", 'PATH="$VBCC/bin:$PATH"; export PATH',
        'VC="$VBCC/bin/vc"; CFG="${VC_CONFIG:-aos68k}"',
        'AMISSL_INC="$VBCC/sdk/AmiSSL-v5/Developer/include"',
        'NET_INC="$VBCC/sdk/Roadshow/netinclude"',
        'MUI_INC="$VBCC/sdk/MUI_3.8/Developer/C/Include"',
        'NDK_INC="$VBCC/sdk/NDK_3.2/Include_H"',
        'RT_INC="$VBCC/targets/m68k-amigaos/include"',
        'RT_LIB="$VBCC/targets/m68k-amigaos/lib"',
        "mkdir -p bin/amiga obj/amiga"] + host) + "\n"
    return script, host_sh, warn


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--verify", action="store_true",
                    help="build the generated script on the host with $VBCC")
    ap.add_argument("--name", help="project name for the header "
                    "(default: name of the project directory)")
    ap.add_argument("project", nargs="?", default=".")
    a = ap.parse_args()
    proj = os.path.abspath(a.project)
    name = a.name or os.path.basename(proj)
    script, host_sh, warn = generate(proj, name)
    with open(os.path.join(proj, "Build-Amiga"), "w", encoding="latin-1") as f:
        f.write(script)
    print(f"Build-Amiga written ({script.count(chr(10)) + 1} lines)")
    for w in warn:
        print("WARNING:", w, file=sys.stderr)
    if a.verify:
        if not os.environ.get("VBCC"):
            sys.exit("--verify needs $VBCC (vbcc cross toolchain root)")
        with tempfile.NamedTemporaryFile("w", suffix=".sh", delete=False) as t:
            t.write(host_sh)
        r = subprocess.run(["sh", t.name], cwd=proj)
        os.unlink(t.name)
        if r.returncode:
            sys.exit("verification FAILED")
        print("verification OK: all programs linked with the host vbcc")


if __name__ == "__main__":
    main()
