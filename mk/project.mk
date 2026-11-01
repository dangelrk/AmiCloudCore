# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# project.mk -- shared top-level build logic for the projects that embed
# AmiCloudCore as the core/ submodule (AmiCloud, AmiOwnCloud, AmiVCS).
#
# The including Makefile sets:
#   PROJECT     project name (dist drawer / archive name)
#   PC_DIRS     directories with a Makefile.pc          (host CLI tools)
#   QT_DIRS     directories with a CMakeLists.txt       (host Qt6 GUIs)
#   AMIGA_DIRS  directories with a Makefile.amiga-cross (AmigaOS binaries)
#   DIST_EXTRA  optional "file:drawer" pairs copied into the Amiga dist
#               drawer, e.g. handler/foo/mount/FOO:Storage/DOSDrivers
# and then: include core/mk/project.mk
#
# Optional in install/amiga/ (not copied into the drawer themselves):
#   icons.spec       Workbench icons for the drawer, see core/tools/mkicon.py
#   aminet.readme    Aminet readme for "make release"; @VERSION@ is replaced
#
# In the drawer's Install-*, Uninstall-* and ReadMe, dist-amiga replaces
# @VERSION@ (e.g. 0.1), @DATE@ (build date, dd.mm.yyyy), @VER_MAJOR@ (0) and
# @VER_REVN@ (revision plus one, 2) -- the shell installers compare
# installed programs against them with "Version ... FILE". Version from
# core/libcloudcore/include/cloudcore/version.h, date = build day. With an
# Install-*.shell, the helper core/tools/amiga/Install-Datei is copied too.
#
# Every sub-Makefile stays usable on its own; this file only loops over
# them. See docs/BUILD.md of the project for the full build instructions.

ifeq ($(wildcard core/libcloudcore/Makefile.pc),)
$(error core/ fehlt -- bitte zuerst "git submodule update --init" ausfuehren)
endif

PREFIX    ?= /usr/local
BUILD_DIR ?= build
DIST_DIR   = $(BUILD_DIR)/dist/$(PROJECT)
RELEASE_DIR = $(BUILD_DIR)/release
VERSION_H  = core/libcloudcore/include/cloudcore/version.h
VERSION   ?= $(shell sed -n 's/^\#define AC_VERSION *"\([^"]*\)".*/\1/p' $(VERSION_H))
# Build date = the day "make" runs (see version.h). Exported, so every
# sub-make passes the same -DAC_BUILD_DATE; override with DATE=dd.mm.yyyy.
DATE      ?= $(shell date +%d.%m.%Y)
export AC_BUILD_DATE := $(DATE)
VER_MAJOR  = $(word 1,$(subst ., ,$(VERSION)))
VER_REVN   = $(shell expr $(word 2,$(subst ., ,$(VERSION))) + 1)
PYTHON    ?= python3
CMAKE     ?= cmake
QT_CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release

# vbcc cross toolchain root (see docs/BUILD.md). VC_CONFIG optionally
# points at an alternative vbcc config file instead of $(VBCC)/config/aos68k
# (needed when that file contains absolute paths of another machine).
VBCC      ?= /opt/vbcc
export VBCC
AMIGA_MAKEFLAGS = $(if $(VC_CONFIG),TARGET=+$(VC_CONFIG))

.PHONY: all pc qt amiga amiga-clean build-amiga-script verify-amiga-script dist-amiga \
        release server-release check-guide fsuae install clean help

all: pc

help:
	@echo "Ziele fuer $(PROJECT):"
	@echo "  make pc                   Kommandozeilen-Programme fuer den Host (bin/pc/)"
	@echo "  make qt                   Qt6-Programme fuer den Host (bin/pc/)"
	@echo "  make amiga                AmigaOS-Programme per vbcc-Cross-Compiler (bin/amiga/)"
	@echo "  make dist-amiga           Amiga-Installationsschublade unter $(DIST_DIR)/"
	@echo "  make release              ... dazu $(RELEASE_DIR)/$(PROJECT)-$(VERSION).lha + .readme"
	@echo "  make server-release       Server-Paket $(RELEASE_DIR)/$(PROJECT)-Server-$(VERSION).tar.gz"
	@echo "  make build-amiga-script   Build-Amiga (natives Amiga-Buildskript) neu erzeugen"
	@echo "  make verify-amiga-script  ... und auf dem Host mit vbcc gegenpruefen"
	@echo "  make check-guide          AmigaGuide-Handbuch(er) in docs/user/ pruefen"
	@echo "  make fsuae [PROGS=...]    Amiga-Programme im Emulator FS-UAE testen"
	@echo "  make install              Host-Programme nach $(PREFIX)/bin installieren"
	@echo "  make clean                alle Build-Ergebnisse entfernen"

pc:
	@set -e; for d in $(PC_DIRS); do \
	    echo "==> $$d (pc)"; $(MAKE) -C $$d -f Makefile.pc; done

qt:
	@set -e; for d in $(QT_DIRS); do \
	    echo "==> $$d (qt)"; \
	    $(CMAKE) -S $$d -B $(BUILD_DIR)/$$(basename $$d) $(QT_CMAKE_FLAGS); \
	    $(CMAKE) --build $(BUILD_DIR)/$$(basename $$d); done

amiga:
	@set -e; PATH="$(VBCC)/bin:$$PATH"; export PATH; \
	for d in $(AMIGA_DIRS); do \
	    echo "==> $$d (amiga)"; \
	    $(MAKE) -C $$d -f Makefile.amiga-cross $(AMIGA_MAKEFLAGS); done

amiga-clean:
	-for d in $(AMIGA_DIRS); do $(MAKE) -C $$d -f Makefile.amiga-cross clean; done

build-amiga-script:
	python3 core/tools/gen-build-amiga.py --name $(PROJECT)

verify-amiga-script:
	VC_CONFIG=$(VC_CONFIG) python3 core/tools/gen-build-amiga.py --name $(PROJECT) --verify

# Amiga installation drawer: programs, AmigaGuide manual(s), example
# configuration, mountlists, the install scripts from install/amiga/
# (Installer script + shell fallback), Workbench icons from
# install/amiga/icons.spec, LICENSE (GPL) and THIRD-PARTY-NOTICES (full
# MIT text of Parson, which the programs contain -- required when
# distributing the binaries). Packed with core/tools/lhapack.py, because
# macOS has no LHA packer (Homebrew's lha is Lhasa and only extracts).
dist-amiga: amiga
	rm -rf $(DIST_DIR) $(DIST_DIR).info
	mkdir -p $(DIST_DIR)/C $(DIST_DIR)/Docs $(DIST_DIR)/Examples
	cp bin/amiga/* $(DIST_DIR)/C/
	cp docs/user/*.guide $(DIST_DIR)/Docs/
	for f in config-examples/*; do case "$$f" in *.md) ;; *) cp "$$f" $(DIST_DIR)/Examples/ ;; esac; done
	for f in install/amiga/*; do case "$${f##*/}" in icons.spec|aminet.readme) ;; *) cp -R "$$f" $(DIST_DIR)/ ;; esac; done
	@for x in $(DIST_EXTRA); do \
	    f=$${x%%:*}; d=$(DIST_DIR)/$${x#*:}; mkdir -p "$$d" && cp "$$f" "$$d/"; done
	cp LICENSE $(DIST_DIR)/LICENSE
	cp core/THIRD-PARTY-NOTICES.txt $(DIST_DIR)/THIRD-PARTY-NOTICES
	@if ls install/amiga/Install-*.shell >/dev/null 2>&1; then \
	    cp core/tools/amiga/Install-Datei $(DIST_DIR)/; fi
	@test -n "$(VERSION)" -a -n "$(DATE)" -a -n "$(VER_REVN)" || \
	    { echo "Version/Datum nicht gefunden ($(VERSION_H))"; exit 1; }
	@for f in $(DIST_DIR)/Install-* $(DIST_DIR)/Uninstall-* $(DIST_DIR)/ReadMe; do \
	    [ -f "$$f" ] || continue; \
	    LC_ALL=C sed -e 's/@VERSION@/$(VERSION)/g' -e 's/@DATE@/$(DATE)/g' \
	        -e 's/@VER_MAJOR@/$(VER_MAJOR)/g' -e 's/@VER_REVN@/$(VER_REVN)/g' \
	        "$$f" > "$$f.tmp" && mv "$$f.tmp" "$$f"; done
	@if [ -f install/amiga/icons.spec ]; then \
	    $(PYTHON) core/tools/mkicon.py --spec install/amiga/icons.spec $(DIST_DIR); fi
	$(PYTHON) core/tools/lhapack.py $(BUILD_DIR)/$(PROJECT).lha $(BUILD_DIR)/dist \
	    $(PROJECT) $(if $(wildcard install/amiga/icons.spec),$(PROJECT).info)

# Release files as uploaded to Aminet/GitHub: the archive with version in
# its name plus the Aminet readme (install/amiga/aminet.readme, Latin-1).
# The archive is checked with lha (Lhasa) when it is installed.
# Every Amiga program is rebuilt, so all of them carry today's date in
# their $VER string -- the day the release is built and tagged.
release:
	@test -n "$(VERSION)" || { echo "VERSION unbekannt (AC_VERSION nicht gefunden)"; exit 1; }
	$(MAKE) amiga-clean
	$(MAKE) dist-amiga
	@echo "Release $(PROJECT) $(VERSION) ($(DATE))"
	mkdir -p $(RELEASE_DIR)
	cp $(BUILD_DIR)/$(PROJECT).lha $(RELEASE_DIR)/$(PROJECT)-$(VERSION).lha
	@if [ -f install/amiga/aminet.readme ]; then \
	    sed 's/@VERSION@/$(VERSION)/g' install/amiga/aminet.readme > $(RELEASE_DIR)/$(PROJECT)-$(VERSION).readme; fi
	@if command -v lha >/dev/null 2>&1; then \
	    lha t $(RELEASE_DIR)/$(PROJECT)-$(VERSION).lha >/dev/null && echo "lha t: Archiv in Ordnung"; fi
	@ls -l $(RELEASE_DIR)/$(PROJECT)-$(VERSION).*

# Server package (projects with a server/ folder): server files, LICENSE,
# SERVER_EXTRA (source[:target]) and INSTALL.md = install/server/README.md
# plus the section SERVER_DOC_HEADING of docs/INSTALL.md.
server-release:
	@test -n "$(VERSION)" || { echo "VERSION unbekannt (AC_VERSION nicht gefunden)"; exit 1; }
	@test -n "$(SERVER_DOC_HEADING)" || { echo "$(PROJECT) hat kein Server-Paket (SERVER_DOC_HEADING fehlt)"; exit 1; }
	python3 core/tools/serverpack.py $(PROJECT) $(VERSION) $(RELEASE_DIR) "$(SERVER_DOC_HEADING)" $(SERVER_EXTRA)

# AmigaGuide syntax/encoding check (nodes, links, Latin-1, line length).
check-guide:
	@set -e; for g in docs/user/*.guide; do \
	    printf '%s: ' "$$g"; python3 core/tools/check-guide.py "$$g"; done

# Copy bin/amiga/ programs into the FS-UAE shared folder and start the
# emulator -- see core/tools/run-fsuae.sh for FSUAE_CONFIG/FSUAE_SHARED.
fsuae:
	core/tools/run-fsuae.sh $(PROGS)

install:
	@test -d bin/pc || { echo "Erst 'make pc' (und ggf. 'make qt') ausfuehren."; exit 1; }
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	@set -e; for f in bin/pc/*; do \
	    if [ -f "$$f" ]; then echo "install $$f"; install -m 755 "$$f" $(DESTDIR)$(PREFIX)/bin/; fi; \
	done

clean:
	-for d in $(PC_DIRS); do $(MAKE) -C $$d -f Makefile.pc clean; done
	-for d in $(AMIGA_DIRS); do $(MAKE) -C $$d -f Makefile.amiga-cross clean; done
	rm -rf bin lib obj $(BUILD_DIR)
	find . -path ./.git -prune -o -type d -name obj -prune -exec rm -rf {} +
