#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
"""mkicon.py -- Workbench-Icons (.info) fuer die Amiga-Installationsschublade.

Aufruf:
    python3 mkicon.py --spec icons.spec ZIELVERZ

icons.spec beschreibt je Zeile ein Icon (Leerzeilen und '#'-Zeilen werden
ignoriert):

    PFAD  TYP  BILD  [tool=STANDARDPROGRAMM] [tt=TOOLTYPE]... [stack=N]
                     [pos=X,Y] [win=X,Y,B,H]

  PFAD   Datei relativ zu ZIELVERZ, zu der das Icon gehoert ("C/foo" ->
         ZIELVERZ/C/foo.info); "." ist die Schublade ZIELVERZ selbst, ihr
         Icon landet als ZIELVERZ.info daneben.
  TYP    disk | drawer | tool | project
  BILD   drawer install uninstall doc guide cloud prefs branch sync mount
  tt=    darf mehrfach vorkommen; Leerzeichen als %20 schreiben.
  pos=   Platz in der Schublade (sonst ordnet Workbench selbst an).
  win=   nur drawer: Position und Groesse des Schubladenfensters.

Das Format ist das klassische OS-2.x/3.x-Icon (DiskObject, Revision 1)
mit zwei Bitplanes in den vier Workbench-Grundfarben 0 grau, 1 schwarz,
2 weiss, 3 blau; ausgewaehlt wird es invertiert dargestellt (GADGHCOMP).
Das zeigt jede icon.library ab 2.04 an, auch OS 3.2 und PeterKs.
"""

import os
import struct
import sys

NOPOS = -0x80000000
TYPES = {"disk": 1, "drawer": 2, "tool": 3, "project": 4}

# '.' grau (Hintergrund)  '#' schwarz  'o' weiss  '+' blau
ART = {
    "drawer": """
....................................
..################################..
..#oooooooooooooooooooooooooooooo#+.
..#o############################o#+.
..#o#++++++++++++++++++++++++++#o#+.
..#o############################o#+.
..#oooooooooooooooooooooooooooooo#+.
..#oooooooooo##########oooooooooo#+.
..#oooooooooo#++++++++#oooooooooo#+.
..#oooooooooo##########oooooooooo#+.
..#oooooooooooooooooooooooooooooo#+.
..#oooooooooooooooooooooooooooooo#+.
..################################+.
...+++++++++++++++++++++++++++++++..
""",
    "install": """
................##..................
...............#oo#.................
...............#oo#.................
...............#oo#.................
...........#####oo#####.............
............#oooooooo#..............
.............#oooooo#...............
..######.......#oo#.......######....
..#++++#........##........#++++#+...
..#++++######################++#+...
..#++++++++++++++++++++++++++++#+...
..#+++++++#ooooooooooooo#++++++#+...
..#+++++++###############++++++#+...
..#++++++++++++++++++++++++++++#+...
..##############################+...
...+++++++++++++++++++++++++++++....
""",
    "uninstall": """
...........##.........##............
............##.......##.............
.............##.....##..............
..............##...##...............
...............##.##................
................###.................
...............##.##................
..######......##...##.....######....
..#++++#.....##.....##....#++++#+...
..#++++######################++#+...
..#++++++++++++++++++++++++++++#+...
..#+++++++#ooooooooooooo#++++++#+...
..#+++++++###############++++++#+...
..#++++++++++++++++++++++++++++#+...
..##############################+...
...+++++++++++++++++++++++++++++....
""",
    "doc": """
......####################..........
......#ooooooooooooooooo#o#.........
......#ooooooooooooooooo#oo#........
......#oo###########oooo#ooo#.......
......#ooooooooooooooooo#####+......
......#oo##############ooooo#+......
......#oooooooooooooooooooooo#+.....
......#oo#############oooooo#+......
......#oooooooooooooooooooooo#+.....
......#oo################ooo#+......
......#oooooooooooooooooooooo#+.....
......#oo##########oooooooooo#+.....
......#oooooooooooooooooooooo#+.....
......########################+.....
.......++++++++++++++++++++++++.....
""",
    "guide": """
......####################..........
......#ooooooooooooooooo#o#.........
......#oooooooo++++oooooo#oo#.......
......#oooooo++++++++ooo#ooo#.......
......#ooooo+++oooo+++oo#####+......
......#oooooooooooo+++ooooooo#+.....
......#oooooooooo++++oooooooo#+.....
......#oooooooo++++oooooooooo#+.....
......#oooooooo+++ooooooooooo#+.....
......#oooooooooooooooooooooo#+.....
......#oooooooo+++ooooooooooo#+.....
......#oooooooo+++ooooooooooo#+.....
......#oooooooooooooooooooooo#+.....
......########################+.....
.......++++++++++++++++++++++++.....
""",
    "cloud": """
..################################..
..#++++++++++++++++++++++++++###o#+.
..################################+.
..#oooooooooooooooooooooooooooooo#+.
..#oooooooooooo######oooooooooooo#+.
..#oooooooooo##++++++##oooooooooo#+.
..#oooooo####++++++++++#ooooooooo#+.
..#ooooo#++++++++++++++####oooooo#+.
..#oooo#++++++++++++++++++++#oooo#+.
..#oooo#++++++++++++++++++++#oooo#+.
..#ooooo####################ooooo#+.
..#oooooooooooooooooooooooooooooo#+.
..################################+.
...+++++++++++++++++++++++++++++++..
""",
    "prefs": """
..################################..
..#++++++++++++++++++++++++++###o#+.
..################################+.
..#oooooooooooooooooooooooooooooo#+.
..#oo#####ooooo#################o#+.
..#oooooooooooo#++++++++##oooooo#o+.
..#oo#####ooooo#################o#+.
..#oooooooooooooooooooooooooooooo#+.
..#oo#####ooooo#################o#+.
..#oooooooooooo#++++++++++++##oo#o+.
..#oo#####ooooo#################o#+.
..#oooooooooooooooooooooooooooooo#+.
..################################+.
...+++++++++++++++++++++++++++++++..
""",
    "branch": """
..################################..
..#++++++++++++++++++++++++++###o#+.
..################################+.
..#oooooooooooooooooooooooooooooo#+.
..#ooo##ooooooooooooooooooooo##oo#+.
..#ooo##ooooooooooooooooooo##oooo#+.
..#ooo##ooooooooooooooooo##oooooo#+.
..#oo####oooooooooooo####oooooooo#+.
..#oo#++#############++++#oooooooo#+
..#oo####oooooooooooo####oooooooo#+.
..#ooo##ooooooooooooooooooooooooo#+.
..#oooooooooooooooooooooooooooooo#+.
..################################+.
...+++++++++++++++++++++++++++++++..
""",
    "sync": """
..################################..
..#++++++++++++++++++++++++++###o#+.
..################################+.
..#oooooooooooooooooooooooooooooo#+.
..#oooooooo##############oooooooo#+.
..#ooooooo#oooooooooooooo#oooo#oo#+.
..#oooooo#ooooooooooooooo######oo#+.
..#ooooooooooooooooooooooooooooooo#+
..#oo######ooooooooooooooo#oooooo#+.
..#oo#oooo#oooooooooooooo#ooooooo#+.
..#oooooooo##############oooooooo#+.
..#oooooooooooooooooooooooooooooo#+.
..################################+.
...+++++++++++++++++++++++++++++++..
""",
    "mount": """
..############################......
..#oooooooooooooooooooooooooo#+.....
..#o########################o#+.....
..#o#++++++++++++++++++++++#o#+.....
..#o#++######++++++++++++++#o#+.....
..#o#++++++++++++++++++++++#o#+.....
..#o########################o#+.....
..#oooooooooooooooooooooooooo#+.....
..#ooo####oooooooooooooo#ooooo#+....
..############################+.....
...++++++++++++++++++++++++++++.....
""",
}

CMAP = {".": 0, "#": 1, "o": 2, "+": 3}


# Die Bilder sind im Raster der klassischen Hires-Pixel (1:2) gezeichnet.
# Heute laufen die meisten Workbenches mit Interlace oder RTG und fast
# quadratischen Pixeln - dort waeren sie platt. Jede Zeile wird deshalb
# verdoppelt (auf PAL ohne Interlace wirken sie dann etwas hoch, wie
# MagicWB-Icons).
VSCALE = 2


def grid(name):
    rows = [r for r in ART[name].strip("\n").split("\n")]
    w = max(len(r) for r in rows)
    out = []
    for r in rows:
        out.extend([[CMAP[c] for c in r.ljust(w, ".")]] * VSCALE)
    return out


def planar(g):
    h, w = len(g), len(g[0])
    wbytes = ((w + 15) // 16) * 2
    out = bytearray()
    for plane in range(2):
        for y in range(h):
            row = bytearray(wbytes)
            for x in range(w):
                if (g[y][x] >> plane) & 1:
                    row[x >> 3] |= 0x80 >> (x & 7)
            out += row
    return bytes(out)


def string_block(s):
    b = s.encode("latin-1") + b"\0"
    return struct.pack(">I", len(b)) + b


def build(kind, art, tool=None, tooltypes=(), stack=0, pos=None, win=None):
    g = grid(art)
    h, w = len(g), len(g[0])
    otype = TYPES[kind]
    x, y = pos if pos else (NOPOS, NOPOS)
    has_dd = otype in (1, 2)
    d = bytearray()
    d += struct.pack(">HH", 0xE310, 1)
    # struct Gadget
    d += struct.pack(">IhhhhHHHIIIiIHI",
                     0, 0, 0, w, h,
                     0x0004,          # GFLG_GADGIMAGE, Auswahl = invertiert
                     0x0003,          # RELVERIFY | GADGIMMEDIATE
                     0x0001,          # BOOLGADGET
                     1, 0, 0, 0, 0, 0,
                     1)               # UserData: Revision 1 (OS 2.x+)
    d += struct.pack(">BB", otype, 0)
    d += struct.pack(">II", 1 if tool else 0, 1 if tooltypes else 0)
    d += struct.pack(">ii", x, y)
    d += struct.pack(">II", 1 if has_dd else 0, 0)
    d += struct.pack(">i", stack)
    assert len(d) == 78
    if has_dd:
        wx, wy, ww, wh = win if win else (50, 30, 400, 160)
        d += struct.pack(">hhhhBBIIIIIIIhhHHH",
                         wx, wy, ww, wh, 0xFF, 0xFF, 0, 0,
                         0, 0, 0, 0, 0, 90, 40, 0xFFFF, 0xFFFF, 1)
        d += struct.pack(">ii", 0, 0)       # dd_CurrentX/Y
    # struct Image
    d += struct.pack(">hhhhhIBBI", 0, 0, w, h, 2, 1, 3, 0, 0)
    d += planar(g)
    if tool:
        d += string_block(tool)
    if tooltypes:
        d += struct.pack(">I", (len(tooltypes) + 1) * 4)
        for t in tooltypes:
            d += string_block(t)
    if has_dd:
        d += struct.pack(">IH", 0, 0)       # dd_Flags, dd_ViewModes
    return bytes(d)


def parse_line(line):
    parts = line.split()
    path, kind, art = parts[0], parts[1], parts[2]
    opts = {"tool": None, "tt": [], "stack": 0, "pos": None, "win": None}
    for p in parts[3:]:
        k, _, v = p.partition("=")
        v = v.replace("%20", " ")
        if k == "tt":
            opts["tt"].append(v)
        elif k == "tool":
            opts["tool"] = v
        elif k == "stack":
            opts["stack"] = int(v)
        elif k == "pos":
            opts["pos"] = tuple(int(n) for n in v.split(","))
        elif k == "win":
            opts["win"] = tuple(int(n) for n in v.split(","))
        else:
            raise ValueError("unbekannte Option %r" % p)
    if kind not in TYPES or art not in ART:
        raise ValueError("unbekannter Typ/Bild in %r" % line)
    return path, kind, art, opts


def main(argv):
    if len(argv) != 4 or argv[1] != "--spec":
        sys.stderr.write(__doc__)
        return 2
    spec, dest = argv[2], argv[3].rstrip("/")
    n = 0
    with open(spec, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            try:
                path, kind, art, o = parse_line(line)
            except (ValueError, IndexError) as e:
                sys.stderr.write("%s:%d: %s\n" % (spec, lineno, e))
                return 1
            if path == ".":
                target = dest + ".info"
            else:
                if not os.path.exists(os.path.join(dest, path)):
                    sys.stderr.write("%s:%d: %s fehlt in %s\n"
                                     % (spec, lineno, path, dest))
                    return 1
                target = os.path.join(dest, path) + ".info"
            with open(target, "wb") as out:
                out.write(build(kind, art, o["tool"], o["tt"], o["stack"],
                                o["pos"], o["win"]))
            n += 1
    print("%d Icons nach %s geschrieben" % (n, dest))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
