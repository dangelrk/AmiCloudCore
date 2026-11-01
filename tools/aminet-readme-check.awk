# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# Checks an Aminet readme (wiki.aminet.net/The_Readme_file) before upload:
#   awk -f aminet-readme-check.awk Projekt.readme
# First line Short:, at most 40 characters; Uploader:, Type: and
# Architecture: in the header; Distribution: only Aminet or NoCD;
# every line at most 78 characters; no CR (LF only). Exit status 1 on
# any error.
BEGIN { err = 0; header = 1 }
/\r/  { printf "%s:%d: CR im Zeilenende (nur LF erlaubt)\n", FILENAME, FNR; err = 1 }
length($0) > 78 { printf "%s:%d: Zeile hat %d Zeichen (hoechstens 78)\n", FILENAME, FNR, length($0); err = 1 }
FNR == 1 && $0 !~ /^Short:/ { printf "%s:1: erste Zeile muss Short: sein\n", FILENAME; err = 1 }
header && /^$/ { header = 0 }
header && /^[A-Za-z]+:/ {
    key = $0; sub(/:.*/, "", key)
    val = $0; sub(/^[A-Za-z]+:[ \t]*/, "", val)
    seen[key] = 1
    if (key == "Short" && length(val) > 40) {
        printf "%s:%d: Short: hat %d Zeichen (hoechstens 40)\n", FILENAME, FNR, length(val); err = 1
    }
    if (key == "Distribution" && val != "Aminet" && val != "NoCD") {
        printf "%s:%d: Distribution: nur Aminet oder NoCD\n", FILENAME, FNR; err = 1
    }
}
END {
    n = split("Short Uploader Type Architecture", need, " ")
    for (i = 1; i <= n; i++)
        if (!(need[i] in seen)) { printf "%s: Feld %s: fehlt\n", FILENAME, need[i]; err = 1 }
    if (!err) printf "Aminet-Readme in Ordnung: %s\n", FILENAME
    exit err
}
