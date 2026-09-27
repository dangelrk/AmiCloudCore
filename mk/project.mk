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
# Every sub-Makefile stays usable on its own; this file only loops over
# them. See docs/BUILD.md of the project for the full build instructions.

ifeq ($(wildcard core/libcloudcore/Makefile.pc),)
$(error core/ fehlt -- bitte zuerst "git submodule update --init" ausfuehren)
endif

PREFIX    ?= /usr/local
BUILD_DIR ?= build
DIST_DIR   = $(BUILD_DIR)/dist/$(PROJECT)
CMAKE     ?= cmake
QT_CMAKE_FLAGS ?= -DCMAKE_BUILD_TYPE=Release

# vbcc cross toolchain root (see docs/BUILD.md). VC_CONFIG optionally
# points at an alternative vbcc config file instead of $(VBCC)/config/aos68k
# (needed when that file contains absolute paths of another machine).
VBCC      ?= /opt/vbcc
export VBCC
AMIGA_MAKEFLAGS = $(if $(VC_CONFIG),TARGET=+$(VC_CONFIG))

.PHONY: all pc qt amiga build-amiga-script verify-amiga-script dist-amiga \
        install clean help

all: pc

help:
	@echo "Ziele fuer $(PROJECT):"
	@echo "  make pc                   Kommandozeilen-Programme fuer den Host (bin/pc/)"
	@echo "  make qt                   Qt6-Programme fuer den Host (bin/pc/)"
	@echo "  make amiga                AmigaOS-Programme per vbcc-Cross-Compiler (bin/amiga/)"
	@echo "  make dist-amiga           Amiga-Installationsschublade unter $(DIST_DIR)/"
	@echo "  make build-amiga-script   Build-Amiga (natives Amiga-Buildskript) neu erzeugen"
	@echo "  make verify-amiga-script  ... und auf dem Host mit vbcc gegenpruefen"
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

build-amiga-script:
	python3 core/tools/gen-build-amiga.py --name $(PROJECT)

verify-amiga-script:
	VC_CONFIG=$(VC_CONFIG) python3 core/tools/gen-build-amiga.py --name $(PROJECT) --verify

# Amiga installation drawer: programs, AmigaGuide manual(s), example
# configuration, mountlists and the install script(s) from install/amiga/.
dist-amiga: amiga
	rm -rf $(DIST_DIR)
	mkdir -p $(DIST_DIR)/C $(DIST_DIR)/Docs $(DIST_DIR)/Examples
	cp bin/amiga/* $(DIST_DIR)/C/
	cp docs/user/*.guide $(DIST_DIR)/Docs/
	for f in config-examples/*; do case "$$f" in *.md) ;; *) cp "$$f" $(DIST_DIR)/Examples/ ;; esac; done
	cp -R install/amiga/. $(DIST_DIR)/
	@for x in $(DIST_EXTRA); do \
	    f=$${x%%:*}; d=$(DIST_DIR)/$${x#*:}; mkdir -p "$$d" && cp "$$f" "$$d/"; done
	cp LICENSE $(DIST_DIR)/LICENSE
	@rm -f $(BUILD_DIR)/$(PROJECT).lha; \
	if command -v lha >/dev/null 2>&1 && \
	   ( cd $(BUILD_DIR)/dist && lha -ao5q ../$(PROJECT).lha $(PROJECT) ) >/dev/null 2>&1; then \
	    echo "Archiv: $(BUILD_DIR)/$(PROJECT).lha"; \
	else rm -f $(BUILD_DIR)/$(PROJECT).lha; \
	    echo "Hinweis: kein packfaehiges lha gefunden -- nur die Schublade $(DIST_DIR) erzeugt."; fi

install:
	@test -d bin/pc || { echo "Erst 'make pc' (und ggf. 'make qt') ausfuehren."; exit 1; }
	mkdir -p $(DESTDIR)$(PREFIX)/bin
	for f in bin/pc/*; do [ -f "$$f" ] && install -m 755 "$$f" $(DESTDIR)$(PREFIX)/bin/; done

clean:
	-for d in $(PC_DIRS); do $(MAKE) -C $$d -f Makefile.pc clean; done
	-for d in $(AMIGA_DIRS); do $(MAKE) -C $$d -f Makefile.amiga-cross clean; done
	rm -rf bin lib obj $(BUILD_DIR)
	find . -path ./.git -prune -o -type d -name obj -prune -exec rm -rf {} +
