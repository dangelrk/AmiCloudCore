# Fremdkomponenten / Third-party notices

AmiCloudCore steht unter der GNU General Public License v3 oder später
(siehe [LICENSE](LICENSE)). Ausgenommen ist folgende eingebettete
Fremdkomponente, die unter ihrer eigenen, GPL-kompatiblen Lizenz steht:

## Parson 1.1.0

- Dateien: `libcloudcore/src/parson.c`, `libcloudcore/include/cloudcore/parson.h`
- Herkunft: https://github.com/kgabis/parson
- Lizenz: MIT, Copyright (c) 2012 - 2020 Krzysztof Gabis. Der vollständige
  Lizenztext steht im Kopf beider Dateien und muss dort erhalten bleiben.
- Lokale Änderung: In `parson.h` wurde die Ersatzdeklaration von `lldiv_t`
  entfernt (kollidierte mit der libc-Deklaration, Parson nutzt sie nicht).
  Details im Kommentar an dieser Stelle.

Alle übrigen Dateien (einschließlich der MD5- und Base64-Implementierung
in `libcloudcore/`) sind eigener Code des Projekts.

Zum Bauen benötigte, aber **nicht** in diesem Repository enthaltene
Software (vbcc, AmigaOS NDK, AmiSSL-SDK, Roadshow-SDK, MUI-SDK, libcurl,
Qt) muss separat unter deren jeweiligen Lizenzen bezogen werden.
