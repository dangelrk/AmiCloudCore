# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# AmiCloudCore -- standalone compile check of the shared libraries.
#
# Normally these libraries are built by the projects that embed this
# repository as their core/ submodule (AmiCloud, AmiOwnCloud, AmiVCS);
# their programs compile/link the sources directly. This Makefile only
# exists so AmiCloudCore itself can be checked in isolation:
#
#   make pc      libcloud*.a for the host into lib/pc/   (needs libcurl)
#   make amiga   all library objects with the vbcc cross compiler
#                (export VBCC=/path/to/vbcc; optional VC_CONFIG=..., see
#                the projects' docs/BUILD.md)
#   make clean

LIBS = libcloudcore libcloudhttp libcloudfs libcloudsync
VBCC ?= /opt/vbcc
export VBCC

.PHONY: all pc amiga clean

all: pc

pc:
	@set -e; for l in $(LIBS); do \
	    echo "==> $$l (pc)"; $(MAKE) -C $$l -f Makefile.pc LIBDIR=$(CURDIR)/lib/pc; done

amiga:
	@set -e; PATH="$(VBCC)/bin:$$PATH"; export PATH; \
	for l in $(LIBS); do \
	    echo "==> $$l (amiga)"; \
	    $(MAKE) -C $$l -f Makefile.amiga-cross $(if $(VC_CONFIG),TARGET=+$(VC_CONFIG)); done

clean:
	-for l in $(LIBS); do $(MAKE) -C $$l -f Makefile.pc clean LIBDIR=$(CURDIR)/lib/pc; \
	    $(MAKE) -C $$l -f Makefile.amiga-cross clean; done
	rm -rf lib
