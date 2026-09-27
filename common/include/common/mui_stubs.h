/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef GUI_AMIGA_MUI_STUBS_H
#define GUI_AMIGA_MUI_STUBS_H

/*
 * mui_stubs.h — vbcc-native inline LVO stubs for the handful of
 * muimaster.library functions the MUI frontends call (AmiCloud Commander,
 * AmiVCS GUI).
 *
 * Why this exists: the MUI 3.8 Developer SDK only ships SAS/C-dialect
 * `#pragma libcall/tagcall` directives (pragmas/muimaster_pragmas.h) and
 * a GCC-only inline/muimaster.h (register-variable __asm syntax vbcc
 * doesn't accept). vbcc silently ignores pragmas it doesn't implement
 * rather than erroring, so `#pragma libcall`/`tagcall` compiled clean but
 * produced plain undefined-symbol link errors for every MUI_* call — no
 * inline LVO jump was ever generated.
 *
 * vbcc's own real convention is "declaration = asm-string" inline stubs,
 * exactly like Include_H/inline/dos_protos.h does for dos.library. This
 * header applies that same pattern to muimaster.library, using the LVO
 * offsets and register assignments documented in
 * mui38dev/MUI/Developer/C/Include/pragma/muimaster_lib.h's `amicall`
 * lines (those are compiler-independent facts about the library's ABI,
 * not tied to any one pragma dialect).
 *
 * SetAttrs()/GetAttr() need no such stub — they're Intuition calls and
 * Include_H/inline/intuition_protos.h already has vbcc-native stubs for
 * them (see <proto/intuition.h>). DoMethod()/DoSuperMethod()/
 * CoerceMethod() need no stub either — they're real linkable amiga.lib
 * object code (clib/alib_protos.h), not per-library LVO calls; the
 * Makefile.amiga-cross links -lamiga for those.
 */

extern struct Library *MUIMasterBase;

Object *__MUI_NewObjectA(__reg("a6") void *, __reg("a0") CONST_STRPTR classname,
                          __reg("a1") struct TagItem *tags) = "\tjsr\t-30(a6)";
#define MUI_NewObjectA(classname, tags) __MUI_NewObjectA(MUIMasterBase, (classname), (tags))

void __MUI_DisposeObject(__reg("a6") void *, __reg("a0") Object *obj) = "\tjsr\t-36(a6)";
#define MUI_DisposeObject(obj) __MUI_DisposeObject(MUIMasterBase, (obj))

LONG __MUI_RequestA(__reg("a6") void *, __reg("d0") APTR app, __reg("d1") APTR win,
                     __reg("d2") LONGBITS flags, __reg("a0") CONST_STRPTR title,
                     __reg("a1") CONST_STRPTR gadgets, __reg("a2") CONST_STRPTR format,
                     __reg("a3") APTR params) = "\tjsr\t-42(a6)";
#define MUI_RequestA(app, win, flags, title, gadgets, format, params) \
    __MUI_RequestA(MUIMasterBase, (app), (win), (flags), (title), (gadgets), (format), (params))

Object *__MUI_MakeObjectA(__reg("a6") void *, __reg("d0") LONG type,
                           __reg("a0") ULONG *params) = "\tjsr\t-120(a6)";
#define MUI_MakeObjectA(type, params) __MUI_MakeObjectA(MUIMasterBase, (type), (params))

/* Variadic wrappers (mui_stubs.c) — build a TagItem/args array from the
 * varargs and forward to the "A" stubs above, exactly like the MUI SDK's
 * own DLib reference stubs for compilers without vararg-pragma
 * support (mui38dev/MUI/Developer/C/DLib/{newobj,Request,MakeObj}.c).
 * Signatures (plain char*, not CONST_STRPTR) match clib/muimaster_protos.h
 * exactly so existing call sites need no extra casts. */
Object *MUI_NewObject(char *classname, Tag tag1, ...);
LONG    MUI_Request(APTR app, APTR win, LONGBITS flags, char *title,
                     char *gadgets, char *format, ...);
Object *MUI_MakeObject(LONG type, ...);

#endif /* GUI_AMIGA_MUI_STUBS_H */
