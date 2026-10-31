# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 Ronny Krueger
#
# Host unit tests for the sync kernel (test_sync.c + fake_transport.c).
# Compiles the kernel sources directly, with AddressSanitizer and
# UndefinedBehaviorSanitizer; the kernel needs no libcloudhttp, so no
# libcurl either.
#
#   make            build and run all tests
#   make run T=...  only tests whose name contains ...
#   make SAN=       build without sanitizers
#   make clean

CC      ?= cc
SAN     ?= -fsanitize=address,undefined -fno-omit-frame-pointer
ROOT     = ../..
INC      = -I. -I../include/cloudsync -I$(ROOT)/libcloudcore/include/cloudcore \
           -I$(ROOT)/libcloudhttp/include/cloudhttp -I$(ROOT)/libcloudfs/include/cloudfs
CFLAGS   = -std=c99 -D_DEFAULT_SOURCE -D_DARWIN_C_SOURCE -g -O1 -Wall -Wextra $(SAN) $(INC)
LDFLAGS  = $(SAN)

SRCS = test_sync.c fake_transport.c \
       ../src/sync.c ../src/syncstate.c \
       $(ROOT)/libcloudcore/src/md5.c $(ROOT)/libcloudcore/src/parson.c \
       $(ROOT)/libcloudcore/src/ui_hooks.c $(ROOT)/libcloudcore/src/progress.c \
       $(ROOT)/libcloudcore/src/cloudtime.c \
       $(ROOT)/libcloudfs/src/localfs_pc.c $(ROOT)/libcloudfs/src/localfs_util.c

OBJDIR = obj
OBJS   = $(patsubst %.c,$(OBJDIR)/%.o,$(notdir $(SRCS)))
BIN    = $(OBJDIR)/test_sync

vpath %.c . ../src $(ROOT)/libcloudcore/src $(ROOT)/libcloudfs/src

.PHONY: all run clean

all: run

run: $(BIN)
	$(BIN) $(T)

$(BIN): $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS)

$(OBJDIR)/%.o: %.c | $(OBJDIR)
	$(CC) $(CFLAGS) -MMD -MP -c -o $@ $<

$(OBJDIR):
	mkdir -p $@

-include $(wildcard $(OBJDIR)/*.d)

clean:
	rm -rf $(OBJDIR)
