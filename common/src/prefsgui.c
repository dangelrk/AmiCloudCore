/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
/*
 * prefsgui.c -- see prefsgui.h.
 */
#include "prefsgui.h"

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>
#include <utility/tagitem.h>
#include <dos/dos.h> /* SIGBREAKF_CTRL_C */
#include <proto/exec.h>
#include <proto/intuition.h>
#include <proto/gadtools.h>

#include <string.h>
#include <stdio.h>

/* Same convention as AmiOwnCloud's apps/amicloud-sync-cx/src/gui.c: this
 * module opens intuition.library/gadtools.library itself (no autoopen),
 * so it owns these globals -- every proto/*.h inline stub for these two
 * libraries calls through them. */
struct IntuitionBase *IntuitionBase;
struct Library        *GadToolsBase;

static struct Screen *g_screen;
static APTR            g_vi;

/* Gadget text is deliberately rendered in topaz.font/8 -- NOT
 * g_screen->Font -- regardless of whatever font the user's Workbench
 * screen actually uses. Every pixel width in this file and in each
 * Prefs program's window-layout code is a hardcoded budget sized for
 * Topaz 8's narrow, fixed-width metrics; a wider (often proportional)
 * screen font made every button/label render far too wide, overflowing
 * its gadget box (reported after real hardware/emulator testing).
 * topaz.font at 8/9pt is a built-in ROM font, always present without
 * diskfont.library -- GadTools resolves/opens it internally from this
 * TextAttr, no explicit OpenFont()/CloseFont() needed here. */
static struct TextAttr g_topaz8 = { (STRPTR)"topaz.font", 8, FS_NORMAL, FPF_ROMFONT };

int prefsgui_screen_open(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 37L);
    if (!IntuitionBase) return 0;

    GadToolsBase = OpenLibrary((CONST_STRPTR)"gadtools.library", 37L);
    if (!GadToolsBase) { prefsgui_screen_close(); return 0; }

    g_screen = LockPubScreen(NULL);
    if (!g_screen) { prefsgui_screen_close(); return 0; }

    g_vi = GetVisualInfo(g_screen, TAG_DONE);
    if (!g_vi) { prefsgui_screen_close(); return 0; }

    return 1;
}

void prefsgui_screen_close(void)
{
    if (g_vi)     { FreeVisualInfo(g_vi); g_vi = NULL; }
    if (g_screen) { UnlockPubScreen(NULL, g_screen); g_screen = NULL; }
    if (GadToolsBase) { CloseLibrary(GadToolsBase); GadToolsBase = NULL; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = NULL; }
}

struct Screen *prefsgui_screen(void) { return g_screen; }
APTR           prefsgui_vi(void)     { return g_vi; }
struct TextAttr *prefsgui_font(void) { return &g_topaz8; }

WORD prefsgui_topborder(void)
{
    if (!g_screen) return 0;
    return g_screen->WBorTop + g_screen->Font->ta_YSize + 2;
}

/* ------------------------------------------------------------------ */
/* Field layout                                                          */
/* ------------------------------------------------------------------ */

void prefsgui_layout_init(PrefsLayout *lay, struct Gadget *glist_context,
                          WORD start_y, WORD field_x, WORD field_w,
                          WORD row_h, WORD gap)
{
    lay->context = glist_context;
    lay->y        = start_y;
    lay->field_x  = field_x;
    lay->field_w  = field_w;
    lay->row_h    = row_h;
    lay->gap      = gap;
}

static void init_ng(struct NewGadget *ng, PrefsLayout *lay, const char *label, UWORD id, WORD width)
{
    memset(ng, 0, sizeof(*ng));
    ng->ng_TextAttr   = &g_topaz8;
    ng->ng_VisualInfo = g_vi;
    ng->ng_LeftEdge   = lay->field_x;
    ng->ng_TopEdge    = lay->y;
    ng->ng_Width      = width;
    ng->ng_Height     = lay->row_h;
    ng->ng_GadgetText = (CONST_STRPTR)label;
    ng->ng_GadgetID   = id;
    ng->ng_Flags      = PLACETEXT_LEFT;
}

struct Gadget *prefsgui_field_string(PrefsLayout *lay, const char *label, UWORD id,
                                     const char *initial, size_t maxlen)
{
    struct NewGadget ng;
    struct Gadget *gad;

    init_ng(&ng, lay, label, id, lay->field_w);
    gad = CreateGadget(STRING_KIND, lay->context, &ng,
                       GTST_String,   (ULONG)(initial ? initial : ""),
                       GTST_MaxChars, (ULONG)maxlen,
                       TAG_DONE);
    if (!gad) return NULL;
    lay->context = gad;
    lay->y += lay->row_h + lay->gap;
    return gad;
}

struct Gadget *prefsgui_field_integer(PrefsLayout *lay, const char *label, UWORD id,
                                      long initial)
{
    struct NewGadget ng;
    struct Gadget *gad;

    init_ng(&ng, lay, label, id, lay->field_w);
    gad = CreateGadget(INTEGER_KIND, lay->context, &ng,
                       GTIN_Number, (ULONG)initial,
                       TAG_DONE);
    if (!gad) return NULL;
    lay->context = gad;
    lay->y += lay->row_h + lay->gap;
    return gad;
}

struct Gadget *prefsgui_field_checkbox(PrefsLayout *lay, const char *label, UWORD id,
                                       int initial_checked)
{
    struct NewGadget ng;
    struct Gadget *gad;

    init_ng(&ng, lay, label, id, 20);
    ng.ng_Flags = PLACETEXT_RIGHT; /* checkbox label reads to the right, not left */
    gad = CreateGadget(CHECKBOX_KIND, lay->context, &ng,
                       GTCB_Checked, (ULONG)(initial_checked ? TRUE : FALSE),
                       TAG_DONE);
    if (!gad) return NULL;
    lay->context = gad;
    lay->y += lay->row_h + lay->gap;
    return gad;
}

struct Gadget *prefsgui_use_save_cancel_row(PrefsLayout *lay, WORD start_x, WORD total_w,
                                            UWORD id_use, UWORD id_save, UWORD id_cancel)
{
    struct NewGadget ng;
    struct Gadget *gad = NULL;
    WORD bw, i;
    static const char *btn_label[3] = { "Benutzen", "Speichern", "Abbrechen" };
    UWORD ids[3];

    ids[0] = id_use; ids[1] = id_save; ids[2] = id_cancel;
    bw = (total_w - 2 * lay->gap) / 3;

    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = &g_topaz8;
    ng.ng_VisualInfo = g_vi;
    ng.ng_TopEdge    = lay->y;
    ng.ng_Width      = bw;
    ng.ng_Height     = lay->row_h;
    ng.ng_Flags      = PLACETEXT_IN;

    for (i = 0; i < 3; i++) {
        ng.ng_LeftEdge   = start_x + (WORD)i * (bw + lay->gap);
        ng.ng_GadgetText = (CONST_STRPTR)btn_label[i];
        ng.ng_GadgetID   = ids[i];
        gad = CreateGadget(BUTTON_KIND, lay->context, &ng, TAG_DONE);
        if (!gad) return NULL;
        lay->context = gad;
    }
    lay->y += lay->row_h + lay->gap;
    return gad;
}

struct Gadget *prefsgui_status_line(PrefsLayout *lay, WORD start_x, WORD total_w,
                                    UWORD id, const char *initial)
{
    struct NewGadget ng;
    struct Gadget *gad;

    memset(&ng, 0, sizeof(ng));
    ng.ng_TextAttr   = &g_topaz8;
    ng.ng_VisualInfo = g_vi;
    ng.ng_LeftEdge   = start_x;
    ng.ng_TopEdge    = lay->y;
    ng.ng_Width      = total_w;
    ng.ng_Height     = lay->row_h;
    ng.ng_GadgetText = NULL;
    ng.ng_GadgetID   = id;
    ng.ng_Flags      = PLACETEXT_IN;

    gad = CreateGadget(TEXT_KIND, lay->context, &ng,
                       GTTX_Text, (ULONG)(initial ? initial : ""), TAG_DONE);
    if (!gad) return NULL;
    lay->context = gad;
    lay->y += lay->row_h + lay->gap;
    return gad;
}

/* ------------------------------------------------------------------ */
/* Read-back                                                              */
/* ------------------------------------------------------------------ */

void prefsgui_get_string(struct Gadget *g, char *out, size_t outsz)
{
    const char *buf = ((struct StringInfo *)g->SpecialInfo)->Buffer;
    strncpy(out, buf ? buf : "", outsz - 1);
    out[outsz - 1] = '\0';
}

/* INTEGER_KIND is a numeric-input STRING_KIND under the hood (same
 * SpecialInfo/StringInfo Intuition already maintains for every string
 * gadget) -- LongInt is "the integer the user has entered" per its own
 * doc comment in intuition.h, updated by Intuition itself as the user
 * types, no gadtools.library version dependency at all. Reads straight
 * off the struct instead of GT_GetGadgetAttrs(GTIN_Number), the same
 * choice prefsgui_get_string() already makes for STRING_KIND's Buffer,
 * for the same reason (see prefsgui_set_checkbox()'s doc comment in
 * prefsgui.h for the checkbox side of this). */
long prefsgui_get_integer(struct Window *win, struct Gadget *g)
{
    (void)win;
    return ((struct StringInfo *)g->SpecialInfo)->LongInt;
}

void prefsgui_set_checkbox(struct Window *win, struct Gadget *g, int checked)
{
    GT_SetGadgetAttrs(g, win, NULL, GTCB_Checked, (ULONG)(checked ? TRUE : FALSE), TAG_DONE);
}

/* ------------------------------------------------------------------ */
/* Modal action loop                                                     */
/* ------------------------------------------------------------------ */

UWORD prefsgui_wait_action(struct Window *win, UWORD *code_out)
{
    struct IntuiMessage *msg;

    for (;;) {
        ULONG sig = Wait((1UL << win->UserPort->mp_SigBit) | SIGBREAKF_CTRL_C);
        if (sig & SIGBREAKF_CTRL_C) return PREFSGUI_ACT_CLOSE;

        while ((msg = GT_GetIMsg(win->UserPort)) != NULL) {
            ULONG class = msg->Class;
            UWORD code  = msg->Code;
            struct Gadget *g = (struct Gadget *)msg->IAddress;
            GT_ReplyIMsg(msg);

            if (class == IDCMP_CLOSEWINDOW) return PREFSGUI_ACT_CLOSE;
            if (class == IDCMP_GADGETUP) {
                if (code_out) *code_out = code;
                return g->GadgetID;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* File-agnostic ENV:/ENVARC: comparison                                 */
/* ------------------------------------------------------------------ */

int prefsgui_files_differ(const char *path_a, const char *path_b)
{
    FILE *fa, *fb;
    int result, ca, cb;

    fa = fopen(path_a, "r");
    if (!fa) return 0;
    fb = fopen(path_b, "r");
    if (!fb) { fclose(fa); return 0; }

    result = 0;
    for (;;) {
        ca = fgetc(fa);
        cb = fgetc(fb);
        if (ca != cb) { result = 1; break; }
        if (ca == EOF) break;
    }

    fclose(fa);
    fclose(fb);
    return result;
}
