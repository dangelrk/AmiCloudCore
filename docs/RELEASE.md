# Release-Checkliste (AmiCloud, AmiOwnCloud, AmiVCS)

Gilt für alle drei Projekte, die diesen Core als Submodul `core/` benutzen.
Erstes Release: **v0.1 vom 31.10.2026** (Tags in allen vier Repos,
GitHub-Releases bei AmiCloud, AmiOwnCloud und AmiVCS). Zweites Release:
**v0.2 vom 31.10.2026** (Sync-Datensicherheit, Mindest-CPU 68020). Drittes Release:
**v0.3 vom 01.11.2026** (Umlaute, Virenprüfung, Konflikte behalten beide
Fassungen, Schutz vor Massenlöschung, Ordnerauswahl, JSON unabhängig vom
Locale, schrittweiser Abgleich, erstes Mac-Programm AmiCloud Desktop). Viertes Release: **v0.4 vom 01.11.2026** (SetDate in den Handlern,
WebDAV erkennt Server-Änderungen auch bei gleichem ETag, Autosync-Intervall
der Commodity, Server-Paket im Amiga-Archiv). Fünftes Release: **v0.5 vom 01.11.2026**
(kein erneuter Anmeldeversuch mit abgelehnten Zugangsdaten). Sechstes Release: **v0.6 vom 02.11.2026**
(AmiVCS: UTF-8-Namen, Schnittstelle für Programme wie die AmigaIDE,
unstage, git-artiges Auschecken, Sperrdatei, Clone-Fix; AmiCloud und
AmiOwnCloud unverändert bis auf die Versionsnummer). Siebtes Release: **v0.7 vom 02.11.2026**
(Namen, die sich nur in Groß-/Kleinschreibung unterscheiden: Handler zeigen
sie als `name_2`, Abgleich/Downloads überspringen sie, auch bei Umlauten;
AmiVCS-Clone räumt bei solchen Namen auf; Commander verschiebt nur nach
erfolgreicher Kopie).

## 1. Version hochsetzen – Pflicht

Die Version steht **nur** in `libcloudcore/include/cloudcore/version.h`:

```c
#define AC_VERSION "0.2"         /* version.revision, beides Zahlen */
```

- Daraus bauen alle Programme ihre `$VER`-Angabe (auch die Handler), und
  `make release` benennt die Archive danach.
- **Das Datum wird nicht gepflegt.** Es ist immer der Tag des Bauens: Die
  Makefiles übergeben `-DAC_BUILD_DATE=TT.MM.JJJJ`, und `make release`
  baut alle Amiga-Programme neu. Ein Release trägt also den Tag, an dem es
  gebaut und getaggt wird. **Bauen und Taggen am selben Tag.** Ein
  anderes Datum erzwingt `make release DATE=TT.MM.JJJJ`. Ohne make (das
  native `Build-Amiga`) steht `__DATE__` im Format „Oct 31 2026“ drin.
- **Ohne höhere Version gilt ein Update als „gleiche Version“.** Die Installer
  vergleichen installierte und neue Programme per `$VER`. Bei gleicher Version
  ersetzen sie nur stillschweigend, bei älterer Archivversion behalten sie
  die installierte Datei.
- Zwei Zahlen, kein Buchstabe und kein dritter Teil (`0.2`, `1.0`, `0.10`).
  `getversion` und `Version … FILE` vergleichen Version und Revision als
  Zahlen, `0.10` ist also neuer als `0.9`.
- Nicht automatisch erfasst, bei Bedarf von Hand anpassen:
  - Beispiele mit Archivnamen: `AmiOwnCloud/docs/INSTALL.md` und
    `AmiOwnCloud-Handbuch.html` (`AmiOwnCloud-0.1.lha`),
    `AmiVCS/docs/INSTALL.md` und `AmiVCS-Handbuch.html` (`AmiVCS-0.1.lha`).
  - Versionszeilen in den Handbüchern: `AmiCloud.guide` und
    `AmiCloud-Handbuch.html` („Programme 0.1 (2026)“), `AmiVCS.guide`
    und `AmiVCS-Handbuch.html` („Programmversion 0.1“),
    `AmiOwnCloud.guide` (Tabelle mit Programm, Version und Datum).
  - Die Server-API-Version von AmiVCS (`{"status":"ok","version":"0.1"}`)
    ist eine eigene Version und hat mit den Amiga-Programmen nichts zu tun.

## 2. Core zuerst, dann Submodule

1. Änderungen im Core (`~/Dev/AmiCloudCore`) committen und pushen.
2. In **jedem** der drei Projekte das Submodul auf denselben Commit setzen:
   ```sh
   git -C core fetch origin && git -C core checkout <core-sha>
   ```
3. Den Submodul-Stand in jedem Projekt committen. Alle drei sollen denselben
   Core-Commit referenzieren.

Dateien nie nur in `core/` der Projekte ändern oder kopieren, ohne sie im
Core zu committen: Sonst ist der Submodul-Stand nicht reproduzierbar.

## 3. Bauen

Toolchain unter `~/Dev/alt/amiga`. Die mitgelieferte vbcc-Config enthält
noch alte Pfade, deshalb eine korrigierte Kopie verwenden:

```sh
sed 's#Dev/amiga#Dev/alt/amiga#g' ~/Dev/alt/amiga/config/aos68k > /tmp/aos68k
make release VBCC=$HOME/Dev/alt/amiga VC_CONFIG=/tmp/aos68k
```

in jedem der drei Projekte. Ergebnis: `build/release/<Projekt>-<version>.lha`
und `.readme`. Dabei prüft `lha t` das Archiv, falls Lhasa installiert ist.

Außerdem vor dem Release:

- `make check-guide`: AmigaGuide-Handbücher (Nodes, Links, Latin-1).
- `make pc`: Host-Build kompiliert noch.
- Haben sich Makefiles geändert (Quellen, Include-Pfade), dann
  `make build-amiga-script` und `make verify-amiga-script` ausführen und das
  geänderte `Build-Amiga` mitcommitten.
- vbcc-Warnungen: Einige Dutzend alte Warnungen sind normal, neue ansehen.
- Prüfen, dass in der Schublade keine Platzhalter übrig sind:
  `grep -l '@[A-Z_]*@' build/dist/*/Install-* build/dist/*/ReadMe` muss leer
  sein.

Das **Gesamtarchiv** AmiSuite hat kein make-Ziel. Erst alle drei
`make release` ausführen, dann im Verzeichnis AmiCloud:

```sh
rm -rf /tmp/suite && mkdir /tmp/suite
for p in AmiCloud AmiOwnCloud AmiVCS; do
  cp -Rp ../$p/build/dist/$p ../$p/build/dist/$p.info /tmp/suite/
done
python3 core/tools/lhapack.py build/release/AmiSuite-<version>.lha /tmp/suite \
  AmiCloud AmiCloud.info AmiOwnCloud AmiOwnCloud.info AmiVCS AmiVCS.info
```

### Server-Pakete (AmiCloud, AmiVCS)

```sh
make server-release   # -> build/release/<Projekt>-Server-<version>.tar.gz
make server-test      # baut das Paket und prüft es (vor dem Hochladen)
```

Enthält `server/` (ohne `config.php`), `LICENSE`, die Zusatzdateien aus
`SERVER_EXTRA` und eine `INSTALL.md`. Die setzt sich zusammen aus
`install/server/README.md` (Inhalt, Aktualisieren, Migrationen je Version,
`@VERSION@`) und dem Server-Abschnitt aus `docs/INSTALL.md`
(`SERVER_DOC_HEADING` im Projekt-Makefile). Kommt eine neue Migration dazu,
in `install/server/README.md` eintragen, ab welcher Version sie nötig ist.
Werkzeug: `tools/serverpack.py`.

Das Server-Paket steckt außerdem in der Amiga-Schublade unter `Server/`
(samt `install/amiga/Server/ReadMe`, Latin-1), damit es mit dem
Aminet-Archiv mitkommt.

`make server-test` (Werkzeug `tools/servertest.py`) prüft zuerst den
Inhalt: `server/` genau wie im Repo, keine `config.php`, keine macOS-Reste,
alles root:root, `php -l` ohne Fehler. Dann installiert es das Paket so,
wie die `INSTALL.md` es beschreibt, in einem Wegwerf-Verzeichnis:
- eigene MariaDB, nur über Socket erreichbar,
- `sql/schema.sql` einspielen,
- DB-Benutzer mit genau den Rechten aus der GRANT-Zeile,
- `config.php` aus `config.php.example`,
- Administrator per `bin/*-admin.php`,
- `php -S`.

Gegen diesen Server läuft `SERVER_TEST` aus dem Projekt-Makefile:
- AmiCloud: `test_api.sh` und `test_changes.py` aus dem Paket,
- AmiVCS: `scripts/test_server.sh`, ein Rundlauf mit der PC-CLI (braucht
  `make pc`).

Voraussetzungen sind `php` mit `pdo_mysql` und MariaDB
(`mariadb-install-db`, `mariadbd`, `mariadb`). Mit `SERVERTEST_KEEP=1`
bleibt das Verzeichnis nach einem Fehler zur Fehlersuche stehen.

### AmiCloud Desktop (macOS)

Nur im Projekt AmiCloud, auf einem Mac mit Qt 6 aus Homebrew:

```sh
scripts/make_desktop_dmg.sh   # -> build/release/AmiCloud-Desktop-<version>.dmg
```

Das Skript bricht ab, wenn das Bundle noch auf Homebrew verweist. Vorher
den Selbsttest gegen einen Testserver laufen lassen (CMake-Ziel
`amicloud-desktop-selftest`, Umgebung `AC_TEST_SERVER/USER/PASS`). Die .dmg
kommt an das AmiCloud-Release. Windows und Linux baut der Autor auf dem
jeweiligen System.

## 4. Im Emulator testen

Amiberry mit `~/Documents/Amiberry/Configurations/A1200.uae` (OS 3.2.3).
Zugriff über die amiagent-MCP-Tools.

- Archive über das Volume `Setup:` (= `~/Amiga/Setup`) übertragen, auf dem
  Amiga mit `lha x` auspacken. Dafür einen eigenen Ordner nehmen, **nicht
  `RAM:t`**, denn das ist `T:`.
- Installer im agent-Shell mit vollem Pfad starten,
  `SYS:System/Installer Install-<P> APPNAME <P> MINUSER AVERAGE DEFUSER AVERAGE`
  (vorher `CD` in die Schublade).
- **Update-Test** über die vorhandene Installation in allen drei Modi:
  - Einsteiger: zeigt nur Meldungen mit `(all)`.
  - Geübter Benutzer: neuere installierte Programme bleiben und werden am
    Ende genannt.
  - Experte: fragt bei jedem vorhandenen Programm mit beiden Versionen.
  - Zum Testen eine „neuere“ Datei vortäuschen:
    `Echo "$VER: amicloud-sync 9.9 (01.01.2030)" >C:amicloud-sync`.
    Echte Datei vorher sichern und danach zurückkopieren.
- Prüfen, dass vorhandene Icons erhalten bleiben (Datum der `.info`-Datei
  unverändert).
- Shell-Variante `Execute Install-<P>.shell` in einem interaktiven Shell-
  Fenster testen. Bei `NewShell … FROM` erscheint die Ausgabe erst
  verzögert. `amiga_type` mit `\n` schickt kein Return, also
  `amiga_key return` verwenden.
- Danach ENV-, ENVARC- und Startup-Dateien des Emulators wieder in den
  Ausgangszustand bringen.

### Fallstricke der Skriptsprachen

- AmigaDOS ersetzt **`$name_mit_unterstrich` nicht**. In Shell-Skripten
  Variablennamen ohne `_` verwenden (oder `${name}`).
- In `.KEY`-Skripten `.BRA {` und `.KET }` setzen, sonst kollidieren `<`
  und `>` mit Umleitungen.
- Installer: `message` erscheint im Einsteigermodus nur mit `(all)`.
  `getversion` liefert `version*65536+revision`, bei fehlender Datei oder
  fehlender `$VER`-Angabe 0. Das Installer-Fenster zeigt nur etwa 10
  zentrierte Zeilen. Lange Erklärungen gehören in `(help …)`.
- `lha` 40.43 (OS 3.2) bricht bei Verzeichniseinträgen (`-lhd-`) ab.
  `lhapack.py` schreibt deshalb keine.

## 5. Commit, Tag, GitHub-Release

Commits und Push nur, wenn der Nutzer es ausdrücklich will.

1. Core committen und pushen, Submodule nachziehen (Abschnitt 2), dann die
   Projekte committen.
2. Annotierte Tags in **allen vier** Repos:
   ```sh
   git tag -a v<version> -m "<Projekt> <version> (<datum>)"
   git push origin main v<version>
   ```
3. GitHub-Releases (die Repos sind derzeit privat):
   ```sh
   gh release create v<version> --repo dangelrk/<Projekt> \
     --title "<Projekt> <version>" --notes-file notes.md \
     build/release/<Projekt>-<version>.lha build/release/<Projekt>-<version>.readme
   ```
   `AmiSuite-<version>.lha` und `AmiCloud-Desktop-<version>.dmg` kommen
   zusätzlich an das AmiCloud-Release, `<Projekt>-Server-<version>.tar.gz`
   an AmiCloud bzw. AmiVCS.
   Release-Text auf Deutsch, Aufbau wie bei v0.1: Kurzbeschreibung,
   Installation, Aktualisieren, Dateien. Zusätzlich: **Was ist neu.**
   Der Core bekommt nur einen Tag, kein Release.
4. Assets prüfen: `gh release view v<version> --repo dangelrk/<Projekt>`.
   Die Größen müssen zu den lokalen Archiven passen.

## 6. Aminet (noch nie gemacht)

Regeln: wiki.aminet.net/Uploading_instructions und
wiki.aminet.net/The_Readme_file.

- **Dateien:** `make release` legt neben den GitHub-Dateien mit Version
  (`<Projekt>-<version>.lha/.readme`) einen Aminet-Satz ohne Version an:
  `build/release/aminet-<version>/<Projekt>.lha` und `<Projekt>.readme`.
  Aminet will einen Namen für alle Versionen; die Version steht nur im
  Readme. Ein Update geht unter demselben Namen hoch, dafür ist kein
  `Replaces:` nötig.
- **Readme** (`install/amiga/aminet.readme`, `@VERSION@` setzt
  `make release` ein). `make release` prüft es mit
  `tools/aminet-readme-check.awk`:
  - `Short:` als erste Zeile, höchstens 40 Zeichen, ohne Version und
    ohne Plattform,
  - `Uploader:`, `Type:` und `Architecture:` vorhanden,
  - `Distribution:` nur `Aminet` oder `NoCD`,
  - Zeilen höchstens 78 Zeichen, nur LF.
- `Uploader:` im Format `amitools@planet-rk.de (Ronny Krueger)`.
- `Requires:` nennt die Aminet-Archive der Pflichtbibliotheken:
  `util/libs/AmiSSL-v5-OS3.lha`, `util/libs/mui38usr.lha` (nur wo MUI
  gebraucht wird). TCP/IP-Stack, 68020 und die optionalen
  `codesets.library` (`util/libs/codesets-6.22.lha`) und `xvs.library`
  (`util/virus/xvslibrary.lha`) stehen im Abschnitt Requirements.
  AmiVCS braucht weder codesets noch xvs.
- Die Quellen sind privat. Die Readmes verweisen deshalb nicht auf GitHub.
  Der Server liegt in AmiCloud und AmiVCS als Schublade `Server` bei.
- Nur `.lha` und `.readme` hochladen. AmiSuite gibt es nur auf GitHub.
- **Hochladen** per FTP (das Web-Formular ist derzeit außer Betrieb):
  ```sh
  cd build/release/aminet-<version>
  curl -T <Projekt>.lha    --user anonymous:amitools@planet-rk.de ftp://main.aminet.net/new/
  curl -T <Projekt>.readme --user anonymous:amitools@planet-rk.de ftp://main.aminet.net/new/
  ```

## 7. Noch offen bzw. ungetestet

- Bisher nur unter OS 3.2.3 im Emulator getestet. OS 3.1 und 3.9 (Installer
  43.x, älteres `Version`, `lha`) sowie echte Hardware sind ungetestet.
- Das Gesamtarchiv AmiSuite wird von Hand gebaut (siehe oben). Ein make-Ziel
  wäre sinnvoll.
