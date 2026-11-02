/* SPDX-License-Identifier: GPL-3.0-or-later
 * Copyright (C) 2026 Ronny Krueger
 *
 * Host unit tests for charset.c: case folding and cs_list_dir() (names
 * that would be one file on the Amiga get "_k" names the handlers can
 * open again).
 */
#include "charset.h"

#include <stdio.h>
#include <string.h>

static int g_fail, g_run;

#define CHECK_STR(got, want) do { g_run++; \
    if (strcmp((got), (want)) != 0) { g_fail++; \
        fprintf(stderr, "%s:%d: '%s' != '%s'\n", __FILE__, __LINE__, (got), (want)); } } while (0)
#define CHECK_INT(got, want) do { g_run++; \
    if ((got) != (want)) { g_fail++; \
        fprintf(stderr, "%s:%d: %d != %d\n", __FILE__, __LINE__, (int)(got), (int)(want)); } } while (0)

#define NAMESZ 108

static void t_fold(void)
{
    char o[64];
    cs_test_mode(CS_TEST_DEFAULT);
    cs_fold_utf8("\xC3\x84PFEL.TXT", o, sizeof(o));          /* ÄPFEL.TXT */
    CHECK_STR(o, "\xC3\xA4pfel.txt");
    cs_fold_utf8("\xC3\x97\xC3\x9F\xE2\x82\xAC", o, sizeof(o)); /* × ß € stay */
    CHECK_STR(o, "\xC3\x97\xC3\x9F\xE2\x82\xAC");
    cs_fold_utf8("AB\xC3", o, sizeof(o));                     /* cut sequence */
    CHECK_STR(o, "ab\xC3");
    cs_fold_utf8("ABCDEF", o, 4);
    CHECK_STR(o, "abc");
    cs_test_mode(CS_TEST_LATIN1);
    cs_fold_local("\xC4PFEL\xD7\xDE", o, sizeof(o));         /* Latin-1: Ä × Þ */
    CHECK_STR(o, "\xE4pfel\xD7\xFE");
    cs_test_mode(CS_TEST_DEFAULT);
    cs_fold_local("\xC3\x96L", o, sizeof(o));                 /* PC: UTF-8 */
    CHECK_STR(o, "\xC3\xB6l");
}

static void t_list_dir_case(void)
{
    const char *names[] = { "abc", "ABC", "Abc", "x.info", "X.info", "abc_2", "other" };
    char out[7][NAMESZ], p[256];
    cs_test_mode(CS_TEST_DEFAULT);
    CHECK_INT(cs_list_dir("/d", "/d", names, 7, out[0], NAMESZ, NAMESZ), 3);
    CHECK_STR(out[1], "ABC");          /* bytewise smallest keeps its name */
    CHECK_STR(out[2], "Abc_3");        /* _2 is a real file here */
    CHECK_STR(out[0], "abc_4");
    CHECK_STR(out[4], "X.info");
    CHECK_STR(out[3], "x_2.info");     /* before the extension */
    CHECK_STR(out[5], "abc_2");
    CHECK_STR(out[6], "other");
    cs_path_to_utf8("/d/abc_4", p, sizeof(p));   CHECK_STR(p, "/d/abc");
    cs_path_to_utf8("/d/Abc_3", p, sizeof(p));   CHECK_STR(p, "/d/Abc");
    cs_path_to_utf8("/d/x_2.info", p, sizeof(p)); CHECK_STR(p, "/d/x.info");
    cs_path_to_utf8("/d/abc_2", p, sizeof(p));   CHECK_STR(p, "/d/abc_2");
    cs_path_to_utf8("/d/ABC", p, sizeof(p));     CHECK_STR(p, "/d/ABC");
}

/* the listing changes: a "_2" name now stands for another file, and a
 * real file takes over a name that was a "_k" one */
static void t_list_dir_moves(void)
{
    const char *a[] = { "Abc", "abc" };
    const char *b[] = { "abc", "Abc_3" };
    char out[2][NAMESZ], p[256];
    cs_test_mode(CS_TEST_DEFAULT);
    CHECK_INT(cs_list_dir("/m", "/m", a, 2, out[0], NAMESZ, NAMESZ), 1);
    CHECK_STR(out[1], "abc_2");
    cs_path_to_utf8("/m/abc_2", p, sizeof(p));   CHECK_STR(p, "/m/abc");
    CHECK_INT(cs_list_dir("/m", "/m", b, 2, out[0], NAMESZ, NAMESZ), 0);
    CHECK_STR(out[0], "abc");                    /* alone again: plain name */
    cs_path_to_utf8("/m/Abc_3", p, sizeof(p));   CHECK_STR(p, "/m/Abc_3");
    /* review 2: the stale "abc_2" must not lead a new file to "abc" */
    cs_path_to_utf8("/m/abc_2", p, sizeof(p));   CHECK_STR(p, "/m/abc_2");
}

/* review 1: an exact name and a lossy one with the same local form --
 * the exact one keeps its name and its own file, in either order */
static void t_exact_vs_lossy(void)
{
    const char *a[] = { "Preis_.txt", "Preis\xE2\x82\xAC.txt" };
    const char *b[] = { "Preis\xE2\x82\xAC.txt", "Preis_.txt" };
    char out[2][NAMESZ], p[256];
    cs_test_mode(CS_TEST_LATIN1);
    CHECK_INT(cs_list_dir("/e1", "/e1", a, 2, out[0], NAMESZ, NAMESZ), 1);
    CHECK_STR(out[0], "Preis_.txt");
    CHECK_STR(out[1], "Preis__2.txt");
    cs_path_to_utf8("/e1/Preis_.txt", p, sizeof(p));   CHECK_STR(p, "/e1/Preis_.txt");
    cs_path_to_utf8("/e1/Preis__2.txt", p, sizeof(p)); CHECK_STR(p, "/e1/Preis\xE2\x82\xAC.txt");
    CHECK_INT(cs_list_dir("/e2", "/e2", b, 2, out[0], NAMESZ, NAMESZ), 1);
    CHECK_STR(out[1], "Preis_.txt");
    CHECK_STR(out[0], "Preis__2.txt");
    cs_path_to_utf8("/e2/Preis_.txt", p, sizeof(p));   CHECK_STR(p, "/e2/Preis_.txt");
    cs_path_to_utf8("/e2/Preis__2.txt", p, sizeof(p)); CHECK_STR(p, "/e2/Preis\xE2\x82\xAC.txt");
    cs_test_mode(CS_TEST_DEFAULT);
}

/* review 3: a new name joining a group does not move the names given
 * out before (an open "P__2" keeps its file); alone, a name gets its
 * plain form back */
static void t_stable(void)
{
    const char *a[] = { "P\xE2\x82\xAC", "P\xE2\x86\x92" };                    /* € → */
    const char *b[] = { "P\xE2\x82\xAC", "P\xE2\x86\x92", "P\xE2\x80\xA0" };  /* € → † */
    const char *c[] = { "P\xE2\x86\x92" };
    char out[3][NAMESZ], p[256];
    cs_test_mode(CS_TEST_LATIN1);
    cs_list_dir("/s1", "/s1", a, 2, out[0], NAMESZ, NAMESZ);
    CHECK_STR(out[0], "P_");
    CHECK_STR(out[1], "P__2");
    cs_list_dir("/s1", "/s1", b, 3, out[0], NAMESZ, NAMESZ);
    CHECK_STR(out[0], "P_");
    CHECK_STR(out[1], "P__2");
    CHECK_STR(out[2], "P__3");
    cs_path_to_utf8("/s1/P__2", p, sizeof(p)); CHECK_STR(p, "/s1/P\xE2\x86\x92");
    cs_path_to_utf8("/s1/P__3", p, sizeof(p)); CHECK_STR(p, "/s1/P\xE2\x80\xA0");
    cs_list_dir("/s1", "/s1", c, 1, out[0], NAMESZ, NAMESZ);
    CHECK_STR(out[0], "P_");
    cs_path_to_utf8("/s1/P_", p, sizeof(p));   CHECK_STR(p, "/s1/P\xE2\x86\x92");
    cs_path_to_utf8("/s1/P__2", p, sizeof(p)); CHECK_STR(p, "/s1/P__2");
    cs_test_mode(CS_TEST_DEFAULT);
}

/* pairs below a directory name that is gone, and an empty listing */
static void t_forget(void)
{
    const char *r1[] = { "Dir", "dir" };
    const char *r2[] = { "dir" };
    const char *x[]  = { "x\xE2\x82\xAC" };
    char out[2][NAMESZ], p[256];
    cs_test_mode(CS_TEST_LATIN1);
    cs_list_dir("/f", "/f", r1, 2, out[0], NAMESZ, NAMESZ);
    CHECK_STR(out[1], "dir_2");
    cs_list_dir("/f/dir_2", "/f/dir", x, 1, out[0], NAMESZ, NAMESZ);
    cs_path_to_utf8("/f/dir_2/x_", p, sizeof(p)); CHECK_STR(p, "/f/dir/x\xE2\x82\xAC");
    cs_list_dir("/f/Dir", "/f/Dir", x, 1, out[0], NAMESZ, NAMESZ);
    cs_list_dir("/f", "/f", r2, 1, out[0], NAMESZ, NAMESZ);           /* "Dir" gone */
    cs_path_to_utf8("/f/dir_2/x_", p, sizeof(p)); CHECK_STR(p, "/f/dir_2/x_");
    cs_path_to_utf8("/f/dir_2", p, sizeof(p));    CHECK_STR(p, "/f/dir_2");
    cs_path_to_utf8("/f/Dir/x_", p, sizeof(p));   CHECK_STR(p, "/f/Dir/x_");  /* no Dir shown */
    cs_list_dir("/g", "/g", x, 1, out[0], NAMESZ, NAMESZ);
    cs_path_to_utf8("/g/x_", p, sizeof(p));       CHECK_STR(p, "/g/x\xE2\x82\xAC");
    cs_list_dir("/g", "/g", NULL, 0, NULL, 0, 0);
    cs_path_to_utf8("/g/x_", p, sizeof(p));       CHECK_STR(p, "/g/x_");
    cs_test_mode(CS_TEST_DEFAULT);
}

/* Umlauts and lossy names on an ISO-8859-1 / ASCII Amiga */
static void t_list_dir_amiga(void)
{
    const char *u[] = { "\xC3\xA4pfel", "\xC3\x84pfel" };              /* äpfel Äpfel */
    const char *l[] = { "Preis\xE2\x82\xAC.txt", "Preis\xE2\x86\x92.txt" }; /* € → */
    char out[2][NAMESZ], p[256];
    cs_test_mode(CS_TEST_LATIN1);
    CHECK_INT(cs_list_dir("/", "/", u, 2, out[0], NAMESZ, NAMESZ), 1);
    CHECK_STR(out[1], "\xC4pfel");
    CHECK_STR(out[0], "\xE4pfel_2");
    cs_path_to_utf8("/\xE4pfel_2", p, sizeof(p)); CHECK_STR(p, "/\xC3\xA4pfel");
    cs_path_to_utf8("/\xC4pfel", p, sizeof(p));   CHECK_STR(p, "/\xC3\x84pfel");
    CHECK_INT(cs_list_dir("/", "/", l, 2, out[0], NAMESZ, NAMESZ), 1);
    CHECK_STR(out[0], "Preis_.txt");
    CHECK_STR(out[1], "Preis__2.txt");
    cs_path_to_utf8("/Preis_.txt", p, sizeof(p));   CHECK_STR(p, "/Preis\xE2\x82\xAC.txt");
    cs_path_to_utf8("/Preis__2.txt", p, sizeof(p)); CHECK_STR(p, "/Preis\xE2\x86\x92.txt");
    cs_test_mode(CS_TEST_DEFAULT);
}

static void t_tilde_fits(void)
{
    const char *n[] = { "aaaaaaaaaa.txt", "AAAAAAAAAA.txt" };
    char out[2][12];
    cs_test_mode(CS_TEST_DEFAULT);
    cs_list_dir("/s", "/s", n, 2, out[0], sizeof(out[0]), sizeof(out[0]));
    /* both are cut to 11 bytes by cs_list_name(); the second still fits */
    CHECK_INT(strstr(out[1], "_") == NULL, 1);
    CHECK_INT((int)strlen(out[0]) <= 11, 1);
    CHECK_INT(strstr(out[0], "_2") != NULL, 1);
}

/* review A: listing one directory again and again does not push the
 * pairs of another one out of the table */
static void t_ring(void)
{
    const char *a[] = { "A\xE2\x82\xAC" };
    const char *b[] = { "B\xE2\x82\xAC", "B\xE2\x86\x92" };
    char out[2][NAMESZ], p[256];
    int k;
    cs_test_mode(CS_TEST_LATIN1);
    cs_list_dir("/ra", "/ra", a, 1, out[0], NAMESZ, NAMESZ);
    for (k = 0; k < 300; k++) cs_list_dir("/rb", "/rb", b, 2, out[0], NAMESZ, NAMESZ);
    cs_path_to_utf8("/ra/A_", p, sizeof(p));   CHECK_STR(p, "/ra/A\xE2\x82\xAC");
    cs_path_to_utf8("/rb/B__2", p, sizeof(p)); CHECK_STR(p, "/rb/B\xE2\x86\x92");
    cs_test_mode(CS_TEST_DEFAULT);
}

int main(void)
{
    cs_open(NULL);
    t_fold();
    t_list_dir_case();
    t_list_dir_moves();
    t_list_dir_amiga();
    t_tilde_fits();
    t_exact_vs_lossy();
    t_stable();
    t_forget();
    t_ring();
    printf("test_charset: %d checks, %d failed\n", g_run, g_fail);
    return g_fail != 0;
}
