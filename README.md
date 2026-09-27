# AmiCloudCore

*Shared C libraries and build tooling for the Amiga cloud projects
[AmiCloud](https://github.com/dangelrk/AmiCloud),
[AmiOwnCloud](https://github.com/dangelrk/AmiOwnCloud) and
[AmiVCS](https://github.com/dangelrk/AmiVCS). Portable C99 for AmigaOS 3.x
(vbcc, bsdsocket + AmiSSL) and macOS/Linux/Windows (libcurl).*

AmiCloudCore enthält den gemeinsamen Unterbau der drei Projekte. Es ist
kein eigenständiges Programm, sondern wird in jedem Projekt als
Git-Submodul unter `core/` eingebunden:

```sh
git clone --recurse-submodules https://github.com/dangelrk/AmiCloud.git
# bzw. in einem bereits geklonten Projekt:
git submodule update --init
```

## Inhalt

| Verzeichnis | Inhalt |
|---|---|
| `libcloudcore/` | Grundtypen und Hilfsfunktionen ohne Netz/Dateisystem: Konfigurationsdateien (`key=value`), Dateieinträge (`entry.h`), MD5, Base64, Zeitumrechnung Amiga/UTC (`cloudtime`), Fortschrittsanzeige, UI-Hooks, Workbench-Start (`wbstartup`), JSON-Parser (Parson, MIT) |
| `libcloudhttp/` | HTTP(S)-Transport: Amiga über bsdsocket.library + AmiSSL 5 (`http_amiga.c`), PC über libcurl (`http_pc.c`). Darauf die Protokoll-Clients: `cmd.c` (AmiCloud-REST-API) und `webdavcmd.c` (WebDAV für OwnCloud/Nextcloud) |
| `libcloudfs/` | Lokales Dateisystem und Wildcards, je eine Amiga- (dos.library) und PC-Variante (POSIX) |
| `libcloudsync/` | Transportneutraler Zwei-Wege-Abgleich mit persistentem Abgleichsstand (`syncstate`); Transport AmiCloud-REST oder WebDAV (`webdav.c`) |
| `common/` | Von mehreren Programmen genutzter Anwendungscode: Dateilisten-Logik der Commander (`panel.c`), GadTools-Baukasten der Prefs-Editoren (`prefsgui.c`), lokale Dateisuche (`search.c`) |
| `mk/project.mk` | Gemeinsame Top-Level-Buildlogik der Projekte (`make pc`, `qt`, `amiga`, `dist-amiga`, `install`, `clean`, `help`) |
| `tools/gen-build-amiga.py` | Erzeugt das native AmigaOS-Buildskript `Build-Amiga` eines Projekts aus dessen `Makefile.amiga-cross`-Dateien und prüft es auf Wunsch mit dem Host-vbcc (`--verify`) |

Jede Bibliothek hat ein `Makefile.pc` (statische Bibliothek
`libcloud*.a`) und ein `Makefile.amiga-cross` (vbcc-Objekte). Die
Amiga-Programme der Projekte übersetzen die Bibliotheksquellen direkt mit
und linken die Objekte, da es auf dem Host kein `oml` gibt.

## Nutzung in einem Projekt

Das Top-Level-`Makefile` eines Projekts legt nur fest, welche Programme
es gibt, und bindet dann die gemeinsame Logik ein:

```make
PROJECT    = AmiCloud
PC_DIRS    = apps/amicloud apps/amicloud-shell apps/cloudsync
QT_DIRS    = apps/amicloud-commander-qt
AMIGA_DIRS = apps/amicloud apps/amicloud-commander-mui handler/cloudhandler
DIST_EXTRA = handler/cloudhandler/mount/CLOUD:Storage/DOSDrivers

include core/mk/project.mk
```

Die Programme erwarten die Bibliotheken unter `../../core/<lib>`, die
Build-Ausgaben landen im Projekt unter `bin/<system>/` und
`lib/<system>/`. Die vollständige Bauanleitung (PC und Amiga, Cross-
Compile mit vbcc und nativ) steht jeweils in `docs/BUILD.md` der
Projekte.

## Eigenständig prüfen

```sh
make pc                           # libcloud*.a nach lib/pc/ (braucht libcurl)
export VBCC=/pfad/zu/vbcc
make amiga                        # alle Objekte mit dem vbcc-Cross-Compiler
make amiga VC_CONFIG=/pfad/aos68k # falls $VBCC/config/aos68k fremde absolute Pfade enthält
make clean
```

## Hinweise für Entwickler

- Änderungen an AmiCloudCore betreffen alle drei Projekte. Nach einem
  Commit hier muss in jedem Projekt der Submodul-Stand aktualisiert werden
  (`git -C core pull && git add core && git commit`).
- Werden Quelldateien einer Bibliothek hinzugefügt oder entfernt, müssen
  die `Makefile.amiga-cross` der Programme angepasst und `Build-Amiga`
  neu erzeugt werden (`make verify-amiga-script` im Projekt).
- Viele Kommentare verweisen auf Planungsdokumente (`docs/05_…`,
  `docs/architecture/…`, `docs/handler-plan.md` usw.) aus der Zeit vor
  der Aufteilung in eigene Repositories. Diese Dokumente liegen jetzt in
  den Projekten unter `docs/design/`, überwiegend in AmiCloud.

## Lizenz

GNU General Public License v3 oder später, siehe [LICENSE](LICENSE).
Parson (JSON-Parser) steht unter der MIT-Lizenz, siehe
[THIRD-PARTY-NOTICES.md](THIRD-PARTY-NOTICES.md).

Copyright (C) 2026 Ronny Krueger
