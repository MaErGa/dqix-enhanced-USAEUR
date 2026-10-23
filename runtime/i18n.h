/* Interface languages. Every text the player reads is written in English in
 * the source, inside tr("..."); tr() gives back the same text in the chosen
 * language, or the English one if that language has no translation for it
 * (so a new or changed English text never shows up blank or broken).
 *
 * The translations live in lang/<code>.po (plain gettext files, editable with
 * Poedit or any text editor). tools/lang_to_c.py turns them into
 * runtime/lang_tables.c, which is part of the build; to add a language, add a
 * .po file and run that tool (docs/TRANSLATING.md).
 *
 * N_("...") marks a text that is translated later (a table of names, a long
 * help text kept in a constant): it returns the text itself; pass it to tr()
 * where it is shown. */
#pragma once

#define N_(s) s

/* The text in the current language (the same pointer when there is none). */
const char* tr(const char* english);

int i18n_count(void);                      /* how many languages (English first) */
const char* i18n_code(int i);              /* "en", "es" */
const char* i18n_name(int i);              /* the language's own name: "English", "Espa\xC3\xB1ol" */
int i18n_find(const char* code);           /* index of a code, -1 if there is none */
int i18n_current(void);
void i18n_set(int i);                      /* switches the language (any thread may then call tr) */

/* The language of the operating system as a two-letter code ("es", "en",
 * "fr", "de", "it", "ja", ...), or "" if it can't be told. */
const char* i18n_os_code(void);
/* The best interface language for the operating system (English if none). */
int i18n_detect(void);
