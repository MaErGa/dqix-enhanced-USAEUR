/* Interface languages (see i18n.h). The tables are generated from the .po files in lang/
 * into lang_tables.c by tools/lang_to_c.py. */
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "i18n.h"
#include "lang_tables.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#define MAX_SLOTS 4096                     /* a power of two, at least twice the longest table */

static int g_cur;
/* two hash tables, so a thread reading while the language changes still sees a whole one */
static const LangEntry* g_slots[2][MAX_SLOTS];
static volatile int g_live;                /* which of the two is in use */

static unsigned hash_str(const char* s) {
    unsigned h = 2166136261u;
    while (*s) { h ^= (unsigned char)*s++; h *= 16777619u; }
    return h;
}

int i18n_count(void) { return g_lang_table_count; }
const char* i18n_code(int i) { return i >= 0 && i < g_lang_table_count ? g_lang_tables[i].code : "en"; }
const char* i18n_name(int i) { return i >= 0 && i < g_lang_table_count ? g_lang_tables[i].name : "English"; }
int i18n_current(void) { return g_cur; }

int i18n_find(const char* code) {
    if (!code) return -1;
    for (int i = 0; i < g_lang_table_count; i++) if (!strcmp(g_lang_tables[i].code, code)) return i;
    return -1;
}

void i18n_set(int i) {
    if (i < 0 || i >= g_lang_table_count) i = 0;
    const LangTable* t = &g_lang_tables[i];
    int next = !g_live;
    memset(g_slots[next], 0, sizeof g_slots[next]);
    for (int k = 0; k < t->n && k < MAX_SLOTS / 2; k++) {
        unsigned h = hash_str(t->e[k].en) & (MAX_SLOTS - 1);
        while (g_slots[next][h]) h = (h + 1) & (MAX_SLOTS - 1);
        g_slots[next][h] = &t->e[k];
    }
    g_cur = i;
    g_live = next;
}

const char* tr(const char* english) {
    if (!english || !g_cur) return english;
    const LangEntry* const* slots = g_slots[g_live];
    unsigned h = hash_str(english) & (MAX_SLOTS - 1);
    while (slots[h]) {
        if (!strcmp(slots[h]->en, english)) return slots[h]->tr[0] ? slots[h]->tr : english;
        h = (h + 1) & (MAX_SLOTS - 1);
    }
    return english;
}

const char* i18n_os_code(void) {
    static char code[8];
    code[0] = 0;
#ifdef _WIN32
    switch (PRIMARYLANGID(GetUserDefaultUILanguage())) {
    case LANG_SPANISH: return "es";
    case LANG_ENGLISH: return "en";
    case LANG_FRENCH: return "fr";
    case LANG_GERMAN: return "de";
    case LANG_ITALIAN: return "it";
    case LANG_JAPANESE: return "ja";
    case LANG_PORTUGUESE: return "pt";
    case LANG_CATALAN: return "ca";
    default: return "";
    }
#else
    const char* v = getenv("LC_ALL");
    if (!v || !*v) v = getenv("LC_MESSAGES");
    if (!v || !*v) v = getenv("LANG");
    if (v && isalpha((unsigned char)v[0]) && isalpha((unsigned char)v[1])) {
        code[0] = (char)tolower((unsigned char)v[0]); code[1] = (char)tolower((unsigned char)v[1]); code[2] = 0;
    }
    return code;
#endif
}

int i18n_detect(void) {
    int i = i18n_find(i18n_os_code());
    return i < 0 ? 0 : i;
}
