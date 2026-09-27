#!/bin/sh
# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# run-fsuae.sh -- kopiert gebaute AmigaOS-Programme aus bin/amiga/ in einen
# mit FS-UAE geteilten Host-Ordner und startet FS-UAE mit einer eigenen
# Konfiguration. Reines Entwickler-Hilfsmittel (siehe docs/BUILD.md,
# Abschnitt "Testen im Emulator").
#
# Aufruf (im Projektverzeichnis, dort wo core/ liegt):
#   make fsuae [PROGS="programm ..."]      oder
#   core/tools/run-fsuae.sh [programm ...]
#   ohne Programme:  ALLE Dateien aus bin/amiga/ werden kopiert
#   mit Programmen:  nur die genannten, z.B.
#                    make fsuae PROGS="amicloud-handler amicloud-worker"
#
# Voraussetzung: vorher "make amiga" (bin/amiga/<programm> existiert).
#
# Konfiguration ueber Umgebungsvariablen:
#   FSUAE_BIN     FS-UAE-Programm. Standard: "fs-uae" aus dem PATH, auf
#                 macOS ersatzweise /Applications/FS-UAE.app/Contents/MacOS/fs-uae
#   FSUAE_CONFIG  FS-UAE-Konfigurationsdatei (*.fs-uae) -- PFLICHT, es gibt
#                 keinen sinnvollen Standard. Darin den geteilten Ordner als
#                 Laufwerk einhaengen, z.B.:
#                     hard_drive_1 = /pfad/zu/shared
#                     hard_drive_1_label = SHARED
#   FSUAE_SHARED  Host-Ordner, der im Emulator als Laufwerk erscheint
#                 (muss zu FSUAE_CONFIG passen). Standard: build/fsuae-shared
#                 im Projektverzeichnis.
#
# Beispiel:
#   export FSUAE_CONFIG=$HOME/amiga/entwicklung.fs-uae
#   export FSUAE_SHARED=$HOME/amiga/shared
#   make fsuae
# Im Amiga-Shell dann z.B.:  CD SHARED:  und das Programm starten.

set -eu

# Projektverzeichnis: dieses Skript liegt in <projekt>/core/tools/.
PROJECT_DIR="${PROJECT_DIR:-$(cd "$(dirname "$0")/../.." && pwd)}"
AMIGA_BIN_DIR="$PROJECT_DIR/bin/amiga"
SHARED_DIR="${FSUAE_SHARED:-$PROJECT_DIR/build/fsuae-shared}"

if [ -z "${FSUAE_BIN:-}" ]; then
    if command -v fs-uae >/dev/null 2>&1; then
        FSUAE_BIN="$(command -v fs-uae)"
    elif [ -x /Applications/FS-UAE.app/Contents/MacOS/fs-uae ]; then
        FSUAE_BIN=/Applications/FS-UAE.app/Contents/MacOS/fs-uae
    else
        FSUAE_BIN=""
    fi
fi

[ -n "$FSUAE_BIN" ] && [ -x "$FSUAE_BIN" ] || {
    echo "FS-UAE nicht gefunden -- FSUAE_BIN auf das fs-uae-Programm setzen." >&2
    exit 1
}
[ -n "${FSUAE_CONFIG:-}" ] || {
    echo "FSUAE_CONFIG ist nicht gesetzt -- Pfad zur .fs-uae-Konfiguration angeben." >&2
    exit 1
}
[ -f "$FSUAE_CONFIG" ] || {
    echo "FS-UAE-Konfiguration nicht gefunden: $FSUAE_CONFIG" >&2
    exit 1
}

mkdir -p "$SHARED_DIR"

if [ "$#" -gt 0 ]; then
    for name in "$@"; do
        src="$AMIGA_BIN_DIR/$name"
        [ -f "$src" ] || { echo "Nicht gefunden: $src (erst \"make amiga\")" >&2; exit 1; }
        cp "$src" "$SHARED_DIR/"
        echo "Kopiert: $src -> $SHARED_DIR/$name"
    done
else
    [ -d "$AMIGA_BIN_DIR" ] && [ -n "$(ls -A "$AMIGA_BIN_DIR" 2>/dev/null)" ] || {
        echo "Keine Programme in $AMIGA_BIN_DIR gefunden (erst \"make amiga\")." >&2
        exit 1
    }
    for src in "$AMIGA_BIN_DIR"/*; do
        [ -f "$src" ] || continue
        cp "$src" "$SHARED_DIR/"
        echo "Kopiert: $src -> $SHARED_DIR/$(basename "$src")"
    done
fi

echo "Starte FS-UAE ($FSUAE_CONFIG) ..."
exec "$FSUAE_BIN" "$FSUAE_CONFIG"
