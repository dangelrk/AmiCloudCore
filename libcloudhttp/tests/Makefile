# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# Host unit tests for libcloudhttp (no network, no libcurl).
#   make         build and run
#   make clean

CC      ?= cc
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
ROOT     = ../..
CFLAGS   = -std=c99 -D_DEFAULT_SOURCE -g -O1 -Wall -Wextra $(SAN) \
           -I../include/cloudhttp -I$(ROOT)/libcloudcore/include/cloudcore
OBJDIR   = obj
BIN      = $(OBJDIR)/test_authguard

.PHONY: all run clean
all: run
run: $(BIN)
	$(BIN)
$(BIN): test_authguard.c ../src/authguard.h $(ROOT)/libcloudcore/src/md5.c | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ test_authguard.c $(ROOT)/libcloudcore/src/md5.c
$(OBJDIR):
	mkdir -p $@
clean:
	rm -rf $(OBJDIR)
