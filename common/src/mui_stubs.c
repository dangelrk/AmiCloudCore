/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * mui_stubs.c — variadic MUI_* wrappers for mui_stubs.h. Same trick the
 * MUI SDK's own DLib reference stubs use for compilers without
 * vararg-pragma support: take the address of the last named parameter to
 * find the start of the (stack-passed, m68k) varargs, no va_list needed.
 */
#include <libraries/mui.h>
#include <proto/intuition.h>
#include "mui_stubs.h"

Object *MUI_NewObject(char *classname, Tag tag1, ...)
{
    return MUI_NewObjectA(classname, (struct TagItem *)&tag1);
}

LONG MUI_Request(APTR app, APTR win, LONGBITS flags, char *title,
                  char *gadgets, char *format, ...)
{
    return MUI_RequestA(app, win, flags, title, gadgets, format,
                         (APTR)(((ULONG)&format) + sizeof(format)));
}

Object *MUI_MakeObject(LONG type, ...)
{
    return MUI_MakeObjectA(type, (ULONG *)(((ULONG)&type) + sizeof(type)));
}
