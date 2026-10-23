/* The shape of the generated translation tables (lang_tables.c, made by
 * tools/lang_to_c.py from the .po files in lang/). */
#pragma once
typedef struct { const char* en; const char* tr; } LangEntry;
typedef struct { const char* code; const char* name; const LangEntry* e; int n; } LangTable;
extern const LangTable g_lang_tables[];     /* [0] is English, with no entries */
extern const int g_lang_table_count;
