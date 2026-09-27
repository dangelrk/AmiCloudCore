#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# check-guide.py -- sanity check for an AmigaGuide file (docs/user/*.guide):
# every @node closed by @endnode, no duplicate nodes, every link and
# @prev/@next/@toc target exists, required header lines present, Latin-1
# (not UTF-8), no tabs, visible line length <= 76.
#
# Usage: python3 core/tools/check-guide.py docs/user/<Projekt>.guide
#        (or "make check-guide" in the project)
import re, sys
path = sys.argv[1]
raw = open(path, 'rb').read()
ok = True
try:
    txt = raw.decode('ascii')
except UnicodeDecodeError:
    txt = raw.decode('latin-1')
    try:
        raw.decode('utf-8'); print("WARN: file is valid UTF-8 (not converted?)")
        if any(b > 0x7f for b in raw): ok = False
    except UnicodeDecodeError:
        pass
nodes, links, open_node = [], [], None
for n, line in enumerate(txt.split('\n'), 1):
    if '\t' in line: print(f"{n}: TAB"); ok = False
    if len(line) > 76 and not line.lstrip().startswith('@{') and '@{' not in line:
        print(f"{n}: line too long ({len(line)})"); ok = False
    vis = re.sub(r'@\{"([^"]*)" link [^}]*\}', r'\1', line)
    if len(vis) > 76: print(f"{n}: visible line too long ({len(vis)})"); ok = False
    for ch in line:
        o = ord(ch)
        if o < 32 or (0x7f <= o < 0xa0): print(f"{n}: control char {o:#x}"); ok = False
    m = re.match(r'@node\s+(\S+)', line, re.I)
    if m:
        if open_node: print(f"{n}: @node inside {open_node}"); ok = False
        open_node = m.group(1); nodes.append(open_node.lower())
    if re.match(r'@endnode', line, re.I):
        if not open_node: print(f"{n}: stray @endnode"); ok = False
        open_node = None
    for lm in re.finditer(r'@\{"[^"]*"\s+link\s+"?([^\s}"]+)', line):
        links.append((n, lm.group(1)))
    nm = re.match(r'@(prev|next|toc)\s+"?([^\s"]+)', line, re.I)
    if nm:
        links.append((n, nm.group(2)))
if open_node: print("unterminated node", open_node); ok = False
if len(set(nodes)) != len(nodes): print("duplicate nodes"); ok = False
for n, t in links:
    if t.lower() not in nodes: print(f"{n}: link to missing node {t}"); ok = False
for h in ('@database', '@$VER:', '@author'):
    if h not in txt: print("missing", h); ok = False
unlinked = set(nodes) - {t.lower() for _, t in links} - {'main'}
if unlinked: print("nodes never linked:", unlinked)
print(f"{len(nodes)} nodes, {len(links)} links:", "OK" if ok else "FEHLER")
sys.exit(0 if ok else 1)
