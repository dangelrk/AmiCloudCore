/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 */
#ifndef PREFSGUI_H
#define PREFSGUI_H

/*
 * prefsgui.h -- shared GadTools scaffolding for this repo's Prefs
 * editors (amicloud-prefs, amivcs-prefs, amicloudsync-prefs; see the
 * repo plan "Drei getrennte Prefs-Programme"). Deliberately independent
 * of cmd.h/http.h/sync.h -- these programs only ever read/write a
 * config file, never talk to a server, so they must not drag that
 * machinery in (Modul-Kapselungsregel: a GUI shell owns no http/sync
 * knowledge of its own).
 *
 * This is a toolbox, not a form-builder framework: each Prefs program
 * still assembles its own window with CreateContext()/CreateGadget()
 * exactly like AmiOwnCloud's apps/amicloud-sync-cx/src/gui.c and its
 * run_pair_dialog() already do -- this file just factors out the parts
 * that were about to be copied three times verbatim: opening the
 * PubScreen/VisualInfo, laying out one labeled field per row, the
 * classic Workbench-Prefs "Benutzen/Speichern/Abbrechen" button row,
 * GT_GetGadgetAttrs read-back, and the modal GADGETUP/CLOSEWINDOW wait
 * loop (modelled on run_pair_dialog()'s).
 *
 * No password masking: this codebase stores passwords in plaintext
 * config files everywhere already (AmigaOS has no keychain), and no
 * existing dialog in this repo (including run_pair_dialog) masks
 * STRING_KIND input -- adding it here would be new, uneven UX rather
 * than matching an existing convention, so plain STRING_KIND is used
 * for password fields too, same as everything else.
 */

#include <exec/types.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <libraries/gadtools.h>
#include <graphics/text.h>

#include <stddef.h>

/* ------------------------------------------------------------------ */
/* PubScreen / VisualInfo -- call prefsgui_screen_open() once at         */
/* startup before building any window, prefsgui_screen_close() once at   */
/* shutdown (also closes gadtools.library/intuition.library). Mirrors    */
/* gui_open()'s/gui_close()'s prologue/epilogue in amicloud-sync-cx.      */
/* ------------------------------------------------------------------ */

int  prefsgui_screen_open(void);
void prefsgui_screen_close(void);

struct Screen *prefsgui_screen(void); /* NULL until screen_open() succeeds */
APTR           prefsgui_vi(void);
WORD           prefsgui_topborder(void); /* WBorTop + Font->ta_YSize + 2 */

/* The fixed topaz.font/8pt TextAttr every gadget in this toolbox is
 * rendered with (see prefsgui.c) -- NOT the public screen's own font.
 * Callers building extra gadgets by hand outside this toolbox (a
 * listview, an extra button row) should use this too, so their text
 * matches the same narrow, predictable metrics every hardcoded pixel
 * width here assumes. */
struct TextAttr *prefsgui_font(void);

/* ------------------------------------------------------------------ */
/* Row-at-a-time field layout. A PrefsLayout tracks the next gadget's    */
/* TopEdge inside a window being built with CreateContext(); the caller  */
/* owns the glist head pointer (from CreateContext(&glist)) exactly as   */
/* gui.c already does -- prefsgui only tracks the running tail so it can */
/* keep chaining CreateGadget() calls. On failure (returns NULL) the     */
/* layout is left untouched; the caller aborts construction and frees    */
/* its own glist head, same convention CreateGadget() itself uses.       */
/* ------------------------------------------------------------------ */

typedef struct {
    struct Gadget *context;  /* running CreateGadget() chain tail */
    WORD y;
    WORD field_x, field_w, row_h, gap;
} PrefsLayout;

void prefsgui_layout_init(PrefsLayout *lay, struct Gadget *glist_context,
                          WORD start_y, WORD field_x, WORD field_w,
                          WORD row_h, WORD gap);

/* Each adds one labeled gadget at the layout cursor and advances it by
 * row_h + gap. `id` is the caller's own GadgetID, used later with
 * GT_GetGadgetAttrs / prefsgui_get_*() and recognised from
 * prefsgui_wait_action()'s return value. */
struct Gadget *prefsgui_field_string  (PrefsLayout *lay, const char *label, UWORD id,
                                       const char *initial, size_t maxlen);
struct Gadget *prefsgui_field_integer (PrefsLayout *lay, const char *label, UWORD id,
                                       long initial);
struct Gadget *prefsgui_field_checkbox(PrefsLayout *lay, const char *label, UWORD id,
                                       int initial_checked);

/* Standard Workbench-Prefs three-button row (Benutzen/Speichern/
 * Abbrechen), placed at the layout cursor starting at `start_x`,
 * spanning `total_w`. Returns the last (Abbrechen) gadget, or NULL on
 * failure. */
struct Gadget *prefsgui_use_save_cancel_row(PrefsLayout *lay, WORD start_x, WORD total_w,
                                            UWORD id_use, UWORD id_save, UWORD id_cancel);

/* A single-line, non-interactive notice/status gadget (TEXT_KIND) at
 * the layout cursor, spanning `total_w` from `start_x`. `initial` is
 * copied in as-is (pass "" for none). Update later with
 * GT_SetGadgetAttrs(gad, win, NULL, GTTX_Text, (ULONG)text, TAG_DONE).
 * Every Prefs program uses this for the "ENV: und ENVARC: unterscheiden
 * sich" notice (see prefsgui_files_differ()) -- loading always prefers
 * ENV: (the live copy) per the repo's USE/SAVE/CANCEL semantics, so a
 * stale ENVARC: is silent unless flagged here. */
struct Gadget *prefsgui_status_line(PrefsLayout *lay, WORD start_x, WORD total_w,
                                    UWORD id, const char *initial);

/* ------------------------------------------------------------------ */
/* Read-back helpers -- GT_GetGadgetAttrs wrappers so callers don't       */
/* repeat the StringInfo/attribute-tag boilerplate (see                   */
/* run_pair_dialog() in amicloud-sync-cx/src/gui.c for the pattern this   */
/* replaces).                                                              */
/* ------------------------------------------------------------------ */

void prefsgui_get_string  (struct Gadget *g, char *out, size_t outsz);
long prefsgui_get_integer (struct Window *win, struct Gadget *g);

/* Checkbox state is NOT read back via GT_GetGadgetAttrs -- unlike
 * STRING_KIND/INTEGER_KIND (both StringInfo-backed, readable straight
 * off the gadget struct), CHECKBOX_KIND has no such direct peek, so it
 * relies on GT_GetGadgetAttrsA/GTCB_Checked (gadtools.library V39+).
 * A 2026-07-15 field report found the checkbox toggling visibly but
 * the saved value never changing, in two independently-written Prefs
 * programs that only share this checkbox code -- so callers now track
 * the boolean themselves (they already have it from the very click
 * event that toggles it) and use this purely to keep the on-screen
 * checkbox in sync with that app-side value, never to read it back. */
void prefsgui_set_checkbox(struct Window *win, struct Gadget *g, int checked);

/* ------------------------------------------------------------------ */
/* Modal action loop -- generalises run_pair_dialog()'s for(;;) Wait()/   */
/* GT_GetIMsg() loop: blocks for the next GADGETUP/CLOSEWINDOW on `win`   */
/* and returns the GadgetID that fired (so both field gadgets, e.g. a     */
/* listview selection, and the button row are visible to the caller), or  */
/* PREFSGUI_ACT_CLOSE for the close gadget / Ctrl-C. `code_out`, if        */
/* non-NULL, receives msg->Code (needed for a listview's selected index). */
/* ------------------------------------------------------------------ */

#define PREFSGUI_ACT_CLOSE ((UWORD)0xFFFF)

UWORD prefsgui_wait_action(struct Window *win, UWORD *code_out);

/* ------------------------------------------------------------------ */
/* File-agnostic ENV:/ENVARC: comparison -- deliberately knows nothing   */
/* about AcConfig or any particular file format (Modul-Kapselungsregel:  */
/* this GUI toolbox owns no config-parsing knowledge), just raw bytes.   */
/* ------------------------------------------------------------------ */

/* 1 if both `path_a` and `path_b` exist AND their raw contents differ;
 * 0 if either is missing (nothing to compare -- not a conflict, just
 * "not saved permanently yet") or both exist and are byte-identical. */
int prefsgui_files_differ(const char *path_a, const char *path_b);

#endif /* PREFSGUI_H */
