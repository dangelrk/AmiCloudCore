# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# Host unit tests for libcloudcore (charset.c), with AddressSanitizer
# and UndefinedBehaviorSanitizer.   make / make SAN= / make clean

CC      ?= cc
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
CFLAGS   = -std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -g -O1 -Wall -Wextra $(SAN) \
           -I../include/cloudcore
OBJDIR   = obj
BIN      = $(OBJDIR)/test_charset

.PHONY: all run clean
all: run
run: $(BIN)
	$(BIN)
$(BIN): test_charset.c ../src/charset.c ../include/cloudcore/charset.h | $(OBJDIR)
	$(CC) $(CFLAGS) -o $@ test_charset.c ../src/charset.c
$(OBJDIR):
	mkdir -p $@
clean:
	rm -rf $(OBJDIR)
