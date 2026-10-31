#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
"""lhapack.py -- LHA-Archive fuer den Amiga bauen (Methode -lh5-).

Aufruf:
    python3 lhapack.py ARCHIV.lha BASISVERZ PFAD [PFAD ...]

Jeder PFAD ist relativ zu BASISVERZ und landet unter genau diesem Namen im
Archiv; Verzeichnisse werden rekursiv eingepackt. Beispiel: aus build/dist mit dem Pfad "AmiCloud" und
"AmiCloud.info" entsteht ein Archiv, das beim Auspacken die Schublade
AmiCloud samt Icon anlegt.

Warum ein eigenes Werkzeug: auf macOS gibt es keinen LHA-Packer - das
"lha" aus Homebrew ist Lhasa und kann nur auspacken. Dieses Skript braucht
nur die Python-Standardbibliothek und schreibt:

  * Level-2-Kopfzeilen (Dateiname/Verzeichnis als Erweiterungskopf,
    Kopf-CRC), Betriebssystemkennung 'A' (Amiga), Zeitstempel in Ortszeit;
    keine eigenen Verzeichniseintraege (-lhd-), siehe add_entry()
  * -lh5- (LZSS mit 8-KB-Fenster + statische Huffman-Bloecke), wie es
    LhA, lha.run, XAD/unlzx-Werkzeuge und Lhasa verstehen; wird eine Datei
    dadurch nicht kleiner, wird sie mit -lh0- unkomprimiert abgelegt

Pruefen: "lha t ARCHIV.lha" (Lhasa) testet alle CRCs.
"""

import os
import struct
import sys

# ---------------------------------------------------------------- CRC-16
_CRC_TABLE = []
for _i in range(256):
    _c = _i
    for _ in range(8):
        _c = (_c >> 1) ^ 0xA001 if _c & 1 else _c >> 1
    _CRC_TABLE.append(_c)


def crc16(data, crc=0):
    tab = _CRC_TABLE
    for b in data:
        crc = (crc >> 8) ^ tab[(crc ^ b) & 0xFF]
    return crc


# ---------------------------------------------------------- Bit-Ausgabe
class BitWriter:
    def __init__(self):
        self.out = bytearray()
        self.acc = 0
        self.n = 0

    def put(self, nbits, value):
        if nbits == 0:
            return
        self.acc = (self.acc << nbits) | (value & ((1 << nbits) - 1))
        self.n += nbits
        while self.n >= 8:
            self.n -= 8
            self.out.append((self.acc >> self.n) & 0xFF)
        self.acc &= (1 << self.n) - 1

    def flush(self):
        if self.n:
            self.out.append((self.acc << (8 - self.n)) & 0xFF)
            self.acc = 0
            self.n = 0
        return bytes(self.out)


# ------------------------------------------------------------- Huffman
def huffman_lengths(freq, maxlen=16):
    """Codelaengen (laengenbegrenzt) wie lha's make_tree/make_len.

    Gibt (lengths, single) zurueck: single ist das einzige benutzte Symbol,
    wenn hoechstens eines vorkommt (dann sind alle Laengen 0)."""
    n = len(freq)
    used = [i for i in range(n) if freq[i]]
    if len(used) < 2:
        return [0] * n, (used[0] if used else 0)
    # Baum per Heap; Blatttiefen ermitteln
    import heapq
    heap = [(freq[i], i, i) for i in used]   # (freq, tiebreak, node)
    heapq.heapify(heap)
    parent = {}
    nxt = n
    while len(heap) > 1:
        f1, _, a = heapq.heappop(heap)
        f2, _, b = heapq.heappop(heap)
        parent[a] = nxt
        parent[b] = nxt
        heapq.heappush(heap, (f1 + f2, nxt, nxt))
        nxt += 1
    root = heap[0][2]
    depth = {root: 0}

    def d(node):
        if node in depth:
            return depth[node]
        depth[node] = d(parent[node]) + 1
        return depth[node]

    lencnt = [0] * (maxlen + 1)
    for i in used:
        lencnt[min(d(i), maxlen)] += 1
    # Kraft-Ungleichung wiederherstellen (lha make_len)
    cum = sum(lencnt[i] << (maxlen - i) for i in range(1, maxlen + 1))
    while cum != (1 << maxlen):
        lencnt[maxlen] -= 1
        for i in range(maxlen - 1, 0, -1):
            if lencnt[i]:
                lencnt[i] -= 1
                lencnt[i + 1] += 2
                break
        cum -= 1
    # haeufigste Symbole bekommen die kuerzesten Codes
    order = sorted(used, key=lambda i: (-freq[i], i))
    lengths = [0] * n
    k = 0
    for ln in range(1, maxlen + 1):
        for _ in range(lencnt[ln]):
            lengths[order[k]] = ln
            k += 1
    return lengths, None


def canonical_codes(lengths):
    codes = [0] * len(lengths)
    code = 0
    for ln in range(1, 17):
        for i, l in enumerate(lengths):
            if l == ln:
                codes[i] = code
                code += 1
        code <<= 1
    return codes


# ----------------------------------------------------------------- LH5
DICBIT = 13
DICSIZ = 1 << DICBIT
MAXMATCH = 256
THRESHOLD = 3
NC = 255 + MAXMATCH + 2 - THRESHOLD     # 510
NP = DICBIT + 1                         # 14
NT = 19
CBIT, TBIT, PBIT = 9, 5, 4
BLOCK_ITEMS = 16000
MAX_CHAIN = 128


def lz_tokens(data):
    """LZSS mit Hash-Ketten und einfacher Lazy-Suche.
    Liefert Liste von int (Literal) oder (laenge, abstand-1)."""
    n = len(data)
    head = {}
    prev = [0] * n
    tokens = []

    def insert(pos):
        if pos + 2 < n:
            key = data[pos:pos + 3]
            prev[pos] = head.get(key, -1)
            head[key] = pos

    def find(pos):
        if pos + 2 >= n:
            return 0, 0
        key = data[pos:pos + 3]
        cand = head.get(key, -1)
        best_len, best_off = 0, 0
        limit = min(MAXMATCH, n - pos)
        chain = MAX_CHAIN
        while cand >= 0 and chain and pos - cand < DICSIZ:
            # billiger Vorab-Test: kann dieser Kandidat den besten schlagen?
            if data[cand + best_len] == data[pos + best_len]:
                ln = 3
                while ln < limit and data[cand + ln] == data[pos + ln]:
                    ln += 1
                if ln > best_len:
                    best_len, best_off = ln, pos - cand - 1
                    if ln == limit:
                        break
            cand = prev[cand]
            chain -= 1
        return best_len, best_off

    pos = 0
    while pos < n:
        ln, off = find(pos)
        insert(pos)
        if ln >= THRESHOLD and pos + 1 < n:
            ln2, off2 = find(pos + 1)
            if ln2 > ln + 1:
                tokens.append(data[pos])
                pos += 1
                ln, off = ln2, off2
                insert(pos)
        if ln >= THRESHOLD:
            tokens.append((ln, off))
            for p in range(pos + 1, pos + ln):
                insert(p)
            pos += ln
        else:
            tokens.append(data[pos])
            pos += 1
    return tokens


def _pbits(p):
    return p.bit_length()


def _write_pt_len(bw, lengths, nn, nbit, special):
    n = nn
    while n > 0 and lengths[n - 1] == 0:
        n -= 1
    bw.put(nbit, n)
    i = 0
    while i < n:
        k = lengths[i]
        i += 1
        if k <= 6:
            bw.put(3, k)
        else:
            bw.put(k - 3, (1 << (k - 3)) - 2)
        if i == special:
            while i < 6 and lengths[i] == 0:
                i += 1
            bw.put(2, i - 3)


def _c_len_runs(c_len):
    """Zerlegt c_len (gekuerzt) in t-Symbole: (sym, extrabits, extraval)."""
    n = NC
    while n > 0 and c_len[n - 1] == 0:
        n -= 1
    out = []
    i = 0
    while i < n:
        k = c_len[i]
        i += 1
        if k == 0:
            count = 1
            while i < n and c_len[i] == 0:
                i += 1
                count += 1
            if count <= 2:
                out.extend([(0, 0, 0)] * count)
            elif count <= 18:
                out.append((1, 4, count - 3))
            elif count == 19:
                out.append((0, 0, 0))
                out.append((1, 4, 15))
            else:
                out.append((2, CBIT, count - 20))
        else:
            out.append((k + 2, 0, 0))
    return n, out


def _send_block(bw, items):
    c_freq = [0] * NC
    p_freq = [0] * NP
    for it in items:
        if isinstance(it, int):
            c_freq[it] += 1
        else:
            c_freq[it[0] - THRESHOLD + 256] += 1
            p_freq[_pbits(it[1])] += 1
    bw.put(16, len(items))

    c_len, c_single = huffman_lengths(c_freq)
    if c_single is None:
        n, runs = _c_len_runs(c_len)
        t_freq = [0] * NT
        for s, _, _ in runs:
            t_freq[s] += 1
        t_len, t_single = huffman_lengths(t_freq)
        if t_single is None:
            _write_pt_len(bw, t_len, NT, TBIT, 3)
            t_code = canonical_codes(t_len)
        else:
            bw.put(TBIT, 0)
            bw.put(TBIT, t_single)
            t_code = [0] * NT
        bw.put(CBIT, n)
        for s, eb, ev in runs:
            bw.put(t_len[s], t_code[s])
            bw.put(eb, ev)
        c_code = canonical_codes(c_len)
    else:
        bw.put(TBIT, 0)
        bw.put(TBIT, 0)
        bw.put(CBIT, 0)
        bw.put(CBIT, c_single)
        c_code = [0] * NC

    p_len, p_single = huffman_lengths(p_freq)
    if p_single is None:
        _write_pt_len(bw, p_len, NP, PBIT, -1)
        p_code = canonical_codes(p_len)
    else:
        bw.put(PBIT, 0)
        bw.put(PBIT, p_single)
        p_code = [0] * NP

    for it in items:
        if isinstance(it, int):
            bw.put(c_len[it], c_code[it])
        else:
            c = it[0] - THRESHOLD + 256
            bw.put(c_len[c], c_code[c])
            p = it[1]
            b = _pbits(p)
            bw.put(p_len[b], p_code[b])
            if b > 1:
                bw.put(b - 1, p)


def lh5_compress(data):
    if not data:
        return b""
    tokens = lz_tokens(data)
    bw = BitWriter()
    for i in range(0, len(tokens), BLOCK_ITEMS):
        _send_block(bw, tokens[i:i + BLOCK_ITEMS])
    return bw.flush()


# ------------------------------------------------------------- Archiv
def _ext(htype, payload):
    return bytes([htype]) + payload


def level2_header(method, packed_size, orig_size, mtime, attr, crc,
                  dirname, filename):
    exts = []
    exts.append(_ext(0x00, b"\0\0"))                # Kopf-CRC, s.u.
    exts.append(_ext(0x01, filename))
    if dirname:
        exts.append(_ext(0x02, dirname.replace(b"/", b"\xff") + b"\xff"))
    # jeder Erweiterungskopf endet mit der Groesse des naechsten
    body = bytearray()
    sizes = [len(e) + 2 for e in exts] + [0]
    for i, e in enumerate(exts):
        body += e + struct.pack("<H", sizes[i + 1])
    base_len = 26
    total = base_len + len(body)
    pad = b""
    if total & 0xFF == 0:           # Level-0/1-Leser sehen 0 als Archivende
        pad = b"\0"
        total += 1
    hdr = bytearray()
    hdr += struct.pack("<H", total)
    hdr += method
    hdr += struct.pack("<IIIBBHB", packed_size, orig_size, mtime, attr, 2,
                       crc, ord("A"))
    hdr += struct.pack("<H", sizes[0])
    hdr += body + pad
    assert len(hdr) == total
    # Kopf-CRC eintragen (Erweiterungskopf 0x00 direkt nach dem Basiskopf)
    hcrc = crc16(bytes(hdr))
    hdr[base_len + 1:base_len + 3] = struct.pack("<H", hcrc)
    return bytes(hdr)


def amiga_time(t):
    """Level-2-Koepfe tragen Unix-Zeit (UTC); Amiga-Packer schreiben dort
    die Ortszeit hinein und lha auf dem Amiga zeigt sie unveraendert an.
    Also die lokale Zeitzonenverschiebung aufaddieren."""
    import time
    lt = time.localtime(t)
    return int(t) + (lt.tm_gmtoff or 0)


def add_entry(out, base, rel, stats):
    full = os.path.join(base, rel)
    mtime = amiga_time(os.path.getmtime(full))
    relb = rel.replace(os.sep, "/").encode("latin-1")
    if os.path.isdir(full):
        # Keine -lhd--Eintraege: das lha von AmigaOS 3.2 (40.43) legt das
        # Verzeichnis an und bricht beim naechsten Eintrag dann mit
        # "Objekt existiert bereits" ab. Verzeichnisse entstehen beim
        # Auspacken ohnehin aus den Pfaden der Dateien.
        for name in sorted(os.listdir(full), key=lambda s: s.lower()):
            if name.startswith(".") or name == "Thumbs.db":
                continue
            add_entry(out, base, os.path.join(rel, name), stats)
        return
    with open(full, "rb") as f:
        data = f.read()
    packed = lh5_compress(data)
    method = b"-lh5-"
    if len(packed) >= len(data):
        packed, method = data, b"-lh0-"
    d, f_ = relb.rsplit(b"/", 1) if b"/" in relb else (b"", relb)
    out += level2_header(method, len(packed), len(data), mtime,
                         0x00, crc16(data), d, f_)
    out += packed
    stats.append((rel, len(data), len(packed), method.decode()))


def main(argv):
    if len(argv) < 4:
        sys.stderr.write(__doc__)
        return 2
    archive, base = argv[1], argv[2]
    out = bytearray()
    stats = []
    for rel in argv[3:]:
        add_entry(out, base, rel, stats)
    out += b"\0"
    with open(archive, "wb") as f:
        f.write(out)
    total = sum(s[1] for s in stats)
    print("%s: %d Dateien, %d -> %d Bytes" % (archive, len(stats), total, len(out)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
