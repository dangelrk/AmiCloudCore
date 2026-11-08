# Arbeiten direkt unter Linux (Manjaro) und Windows 11

Diese Anleitung richtet einen Manjaro- bzw. Windows-11-Rechner (x64) so
ein, dass Claude Code dort die PC-Fassungen von AmiCloud, AmiOwnCloud,
AmiVCS und AmiDevIDE bauen, testen und weiterentwickeln kann. Die
Amiga-Programme werden weiterhin auf dem Mac gebaut und released
([RELEASE.md](RELEASE.md)).

## 1. Ausgangslage – was erwartet einen

| | Linux | Windows |
|---|---|---|
| Kommandozeilenprogramme (`make pc`) | sollte gehen, nie gebaut | nur in der POSIX-Umgebung „MSYS“ (braucht `msys-2.0.dll`), nie gebaut |
| Qt-Programme (`make qt`): Commander, AmiCloud Desktop, AmiVCS-GUI | sollte gehen, nie gebaut | **erst nach dem Kern-Port** |
| AmiCloud Desktop: Autostart, Tray, Schlüsselbund | fehlt (Phase 8) | fehlt (Phase 7) |
| AmiDevIDE (Toolchain + Qt-IDE) | „nicht end-to-end getestet“ | „nicht end-to-end getestet“ |
| Server-Tests (`make server-test`) | ja (PHP + MariaDB) | nicht nötig |

**Linux** ist also vor allem „bauen, Fehler beheben, testen“.
**Windows** ist ein Portierungsprojekt: `docs/design/desktop-sync-plan.md`
in AmiCloud, Abschnitt „Windows“ und Phase 7, beschreibt es –
`localfs_win.c` (UTF-8 ↔ UTF-16, `_wfopen`, `MoveFileExW`, `\\?\`-Pfade),
alle direkten `fopen`/`remove`/`rename` im Kern über libcloudfs, Dateigrößen
als 64 Bit (`long` ist unter Windows 32 Bit), danach Installer mit
`windeployqt` + Inno Setup. Linux-Pakete: AppImage, Secret Service,
StatusNotifier (Phase 8).

## 2. Gemeinsam für beide Systeme

- **GitHub-Zugang:** Die Repositories sind öffentlich (seit 08.11.2026),
  Klonen geht ohne Anmeldung. Zum Pushen mit der GitHub-CLI
  anmelden (`gh auth login`, „HTTPS“, „Login with a web browser“) und
  `gh auth setup-git` aufrufen, dann klappen `git clone/pull/push`.
- **Git-Identität** wie auf dem Mac:
  ```sh
  git config --global user.name  "Ronny Krueger"
  git config --global user.email "rk_darkangel@web.de"
  ```
- **Zeilenenden nie umwandeln.** Viele Dateien sind Latin-1 mit LF
  (AmigaGuide, ReadMe, Installer-Skripte), `make release` prüft die
  Aminet-Readmes auf reines LF. Unter Windows deshalb **vor dem Klonen**:
  ```sh
  git config --global core.autocrlf false
  ```
- **Verzeichnislayout** wie auf dem Mac, alle fünf Repositories
  nebeneinander, Pfad ohne Leerzeichen:
  ```sh
  mkdir -p ~/Dev && cd ~/Dev            # Windows: C:\Dev (siehe unten)
  for r in AmiCloudCore AmiCloud AmiOwnCloud AmiVCS AmiDevIDE; do
      git clone --recurse-submodules https://github.com/dangelrk/$r.git
  done
  ```
  Das Submodul `core/` zeigt relativ auf `../AmiCloudCore.git` und kommt
  beim Klonen mit.
- **Claude Code** installieren (Befehle unten je System), im jeweiligen
  Projektordner `claude` starten und anmelden.
- **Was Claude dort nicht weiß:** Das Gedächtnis aus den Mac-Sitzungen
  wandert nicht mit. Alles Nötige steht in den Repositories:
  `docs/BUILD.md` je Projekt, `docs/design/desktop-sync-plan.md` (AmiCloud),
  diese Datei und `RELEASE.md`. Beim ersten Auftrag darauf verweisen
  (Vorschläge in Abschnitt 5).
- **Abgleich mit dem Mac:** Vor jeder Sitzung `git pull` in allen
  Repositories, danach committen und pushen. Ändert sich etwas in `core/`,
  zuerst in AmiCloudCore committen und pushen, dann das Submodul in den
  Projekten nachziehen (RELEASE.md, Abschnitt 2).

## 3. Manjaro x64

### 3.1 Pakete

```sh
sudo pacman -Syu
sudo pacman -S --needed base-devel git github-cli cmake ninja pkgconf curl \
    qt6-base qt6-tools python php mariadb lhasa xorg-server-xvfb imagemagick
```

| Paket | Wofür |
|---|---|
| `base-devel` (gcc, make), `pkgconf`, `curl` | `make pc` (libcurl mit Headern ist in `curl` enthalten) |
| `cmake`, `ninja`, `qt6-base`, `qt6-tools` | `make qt`, Qt-IDE, Qt-Tests |
| `python` (≥ 3.11), `lhasa` | AmiDevIDE-Bootstrap, Werkzeuge in `tools/` |
| `php`, `mariadb` | `make server-test` (startet eine eigene Wegwerf-MariaDB, kein Systemdienst nötig) |
| `xorg-server-xvfb`, `imagemagick` | Qt-Programme ohne Bildschirm starten und Screenshots machen |

PHP braucht für die Server-Tests den MySQL-Treiber. In `/etc/php/php.ini`
die Zeilen `extension=pdo_mysql` und `extension=mysqli` einkommentieren
(`php -m | grep -i mysql` muss beide zeigen).

Optional: FS-UAE aus dem AUR (`yay -S fs-uae`) für die Emulator-Ziele der
AmiDevIDE, `linuxdeploy` + `linuxdeploy-plugin-qt` aus dem AUR für
AppImages (Phase 8).

### 3.2 Claude Code

```sh
curl -fsSL https://claude.ai/install.sh | bash
```

### 3.3 Bauen und testen

```sh
cd ~/Dev/AmiCloudCore && make test          # Kern-Tests (ASan/UBSan)
cd ~/Dev/AmiCloud     && make pc && make qt # CLI, Commander, AmiCloud Desktop
cd ~/Dev/AmiOwnCloud  && make pc
cd ~/Dev/AmiVCS       && make pc && make qt && make test
cd ~/Dev/AmiVCS/libs/libcloudvcs/tests && make
cd ~/Dev/AmiCloud     && make server-test   # Server-Paket mit Wegwerf-DB
cd ~/Dev/AmiVCS       && make server-test
```

Ergebnisse liegen in `bin/pc/` bzw. `build/<programm>/`. Qt-Tests laufen
ohne Bildschirm mit `QT_QPA_PLATFORM=offscreen`; echte Fenster mit
`xvfb-run -a <programm>` und Screenshot per `import -window root bild.png`.

AmiDevIDE: `cd ~/Dev/AmiDevIDE && ./install.sh` (prüft die
Voraussetzungen, baut Toolchain, Qt-IDE und Tests, installiert unter Linux
aber nichts selbst). Einzelschritte in `INSTALL.md`, Abschnitt 6.

## 4. Windows 11 x64

### 4.1 Grundausstattung

1. **Git for Windows** (`https://git-scm.com/download/win`) – Claude Code
   braucht dessen Git Bash. Bei der Frage nach Zeilenenden
   „Checkout as-is, commit as-is“ wählen.
2. **Claude Code** in PowerShell:
   ```powershell
   irm https://claude.ai/install.ps1 | iex
   ```
3. **MSYS2** (`https://www.msys2.org/`) nach `C:\msys64` installieren,
   danach in der Shell „MSYS2 MSYS“ einmal `pacman -Syu` (die Shell
   schließt sich dabei, neu öffnen und wiederholen, bis nichts mehr kommt).
4. **GitHub-CLI**: `winget install GitHub.cli`, dann `gh auth login` und
   `gh auth setup-git` (Abschnitt 2).
5. **Defender:** `C:\Dev` und `C:\msys64` von der Echtzeitprüfung
   ausnehmen (Einstellungen › Windows-Sicherheit › Viren- & Bedrohungsschutz
   › Ausschlüsse). Sonst ist jeder Build langsam, und frisch gebaute
   `.exe`-Dateien landen gelegentlich in der Quarantäne.

### 4.2 MSYS2-Pakete

Zwei Umgebungen, weil die Kommandozeilenprogramme POSIX brauchen
(`termios` in der Shell) und die Qt-Programme native Windows-Programme
sind:

```sh
# Umgebung MSYS: Kommandozeilenprogramme (POSIX-Emulation)
pacman -S --needed gcc make pkgconf libcurl-devel git python

# Umgebung UCRT64: Qt-Programme, AmiDevIDE, später der native Port
pacman -S --needed mingw-w64-ucrt-x86_64-{gcc,cmake,ninja,pkgconf,curl,qt6-base,qt6-tools,python,7zip}
```

### 4.3 Klonen

Nach `C:\Dev` – erreichbar als `/c/Dev` in Git Bash und in MSYS2, ohne
Leerzeichen. Die Befehle aus Abschnitt 2 in Git Bash ausführen
(`mkdir -p /c/Dev && cd /c/Dev`), vorher `core.autocrlf false` setzen.

### 4.4 Wie Claude die MSYS2-Umgebungen aufruft

Claude Code arbeitet in Git Bash. Ein Befehl läuft in der richtigen
MSYS2-Umgebung, wenn man deren Login-Shell mit `MSYSTEM` aufruft;
`CHERE_INVOKING=1` behält das aktuelle Verzeichnis:

```sh
# Kommandozeilenprogramme
MSYSTEM=MSYS   CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc 'make pc'
# Qt-Programme
MSYSTEM=UCRT64 CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc 'make qt'
```

Diese beiden Zeilen beim ersten Auftrag mitgeben (Abschnitt 5) oder in
eine `CLAUDE.local.md` im Projektordner schreiben.

### 4.5 Grenzen unter Windows

- Ein Programm aus der MSYS-Umgebung läuft nur mit `msys-2.0.dll` –
  zum Entwickeln gut, zum Weitergeben nicht. Verteilbare Programme
  kommen erst mit dem nativen Port (UCRT64).
- Fenster und Screenshots: Claude kann Programme starten und die Qt-Tests
  im Modus `offscreen` laufen lassen; das Bild des Desktops sieht es nur,
  wenn du angemeldet bist und es Screenshots per PowerShell macht.
- Server-Tests sind unter Windows nicht vorgesehen; das bleibt Linux oder
  Mac.

## 5. Erste Aufträge an Claude

**Manjaro**, im Ordner `~/Dev/AmiCloud`:

> Lies `core/docs/PC-ENTWICKLUNG.md` und `docs/BUILD.md`. Baue auf diesem
> Manjaro-Rechner nacheinander AmiCloudCore (`make test`), AmiCloud,
> AmiOwnCloud und AmiVCS (`make pc`, `make qt`, Tests, `make server-test`)
> und behebe, was unter Linux nicht baut oder nicht besteht. Der Mac-Build
> muss dabei unverändert funktionieren. Danach die `BUILD.md`-Abschnitte
> zu Linux auf Manjaro aktualisieren („getestet“). Commits erst nach
> Rückfrage.

**Windows 11**, im Ordner `C:\Dev\AmiCloud`:

> Lies `core/docs/PC-ENTWICKLUNG.md`, `docs/BUILD.md` und in
> `docs/design/desktop-sync-plan.md` den Abschnitt „Windows“ und Phase 7.
> MSYS2-Befehle rufst du so auf:
> `MSYSTEM=MSYS CHERE_INVOKING=1 /c/msys64/usr/bin/bash -lc '…'`
> (Kommandozeile) bzw. mit `MSYSTEM=UCRT64` (Qt, nativ).
> Schritt 1: `make pc` in der Umgebung MSYS zum Laufen bringen und die
> Programme gegen einen Testserver prüfen. Schritt 2: einen Plan für den
> nativen Kern-Port (Phase 7) schreiben, bevor du Code änderst. Commits
> erst nach Rückfrage.

**AmiDevIDE** (beide Systeme), im Ordner der IDE:

> Lies `INSTALL.md` und `CLAUDE.md`. Richte die Entwicklungsumgebung mit
> `install.sh` bzw. den Einzelschritten aus Abschnitt 6 ein, bring
> Toolchain, Qt-IDE und `ctest` zum Laufen und trage in `INSTALL.md` ein,
> was auf diesem System anders war.

Für Tests gegen einen Server: nie den Produktivserver beschreiben. Unter
Linux startet `make server-test` einen Wegwerfserver; für längere Tests
`python3 core/tools/servertest.py <Server-Paket> "sleep 100000"` (Paket
vorher mit `make server-release` bauen), Adresse und Zugangsdaten stehen
in der Ausgabe.
