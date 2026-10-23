#!/usr/bin/env python3
"""Translations: the .po files in lang/  ->  runtime/lang_tables.c

    python3 tools/lang_to_c.py            write runtime/lang_tables.c
    python3 tools/lang_to_c.py --check    list texts without a translation, texts
                                          no longer in the code, and %-mismatches

The code writes every text in English inside tr("..."), N_("...") or, for the
Windows menus and dialogs, item(...), sub(...) and W("..."). This tool finds
those texts in runtime/*.c and compares them with each .po file. Nothing here
runs while the game is built: lang_tables.c is committed, so the build needs no
extra tools. Run this after changing a .po file or an English text.
"""
import glob, os, re, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
STR = r'(?:"(?:[^"\\]|\\.)*"\s*)+'          # one or more adjacent C string literals
PATTERNS = [
    re.compile(r'\b(?:tr|N_|W)\s*\(\s*(' + STR + r')'),
    # menu_win32.c: item(menu, ID, "text") and sub(menu, "text")
    re.compile(r'\b(?:item|sub)\(\s*[^,()"]+,\s*(?:[^,()"]+,\s*)?(' + STR + r')\)'),
]
ESC = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', '"': '"', "'": "'"}


def c_decode(lit):
    """The text of a run of C string literals ("a" "b" -> ab)."""
    out = []
    for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', lit):
        s = m.group(1)
        i = 0
        while i < len(s):
            c = s[i]
            if c == '\\':
                i += 1
                if s[i] not in ESC:
                    raise ValueError('unsupported escape \\%s in %r' % (s[i], lit[:60]))
                out.append(ESC[s[i]])
            else:
                out.append(c)
            i += 1
    return ''.join(out)


def c_encode(s):
    o = []
    for ch in s:
        if ch == '\\': o.append('\\\\')
        elif ch == '"': o.append('\\"')
        elif ch == '\n': o.append('\\n')
        elif ch == '\t': o.append('\\t')
        elif ch == '\r': o.append('\\r')
        elif ord(ch) < 32: raise ValueError('control character in %r' % s)
        else: o.append(ch)
    return '"' + ''.join(o) + '"'


def keys_in_code():
    """{english text: [file:line, ...]} from every source file."""
    found = {}
    for path in sorted(glob.glob(os.path.join(ROOT, 'runtime', '*.c')) + glob.glob(os.path.join(ROOT, 'runtime', '*.h'))):
        name = os.path.basename(path)
        if name in ('lang_tables.c', 'i18n.h', 'stb_truetype.h', 'stb_image.h'):
            continue
        src = open(path, encoding='utf-8').read()
        for pat in PATTERNS:
            for m in pat.finditer(src):
                text = c_decode(m.group(1))
                line = src.count('\n', 0, m.start()) + 1
                found.setdefault(text, []).append('%s:%d' % (name, line))
    return found


def read_po(path):
    """(header dict, {msgid: msgstr}) from a gettext .po file."""
    entries, cur, field = [], {}, None
    for raw in open(path, encoding='utf-8'):
        line = raw.strip()
        if not line or line.startswith('#'):
            if not line and cur.get('msgid') is not None:
                entries.append(cur); cur = {}; field = None
            continue
        m = re.match(r'(msgid|msgstr)\s+("(?:[^"\\]|\\.)*")$', line)
        if m:
            field = m.group(1)
            if field == 'msgid' and cur.get('msgid') is not None:
                entries.append(cur); cur = {}
            cur[field] = c_decode(m.group(2))
        elif line.startswith('"') and field:
            cur[field] += c_decode(line)
        else:
            raise ValueError('%s: cannot read line %r' % (path, raw))
    if cur.get('msgid') is not None:
        entries.append(cur)
    header, table = {}, {}
    for e in entries:
        if e['msgid'] == '':
            for h in e.get('msgstr', '').split('\n'):
                if ':' in h:
                    k, v = h.split(':', 1); header[k.strip()] = v.strip()
        else:
            table[e['msgid']] = e.get('msgstr', '')
    return header, table


def specs(s):
    return sorted(re.findall(r'%[-+ #0]*\d*(?:\.\d+)?(?:ll|l|h|z)?[sdiuxXcfgp%]', s))


def accel(s):
    """The &-mnemonic letters of a menu text (not the part after the tab)."""
    return re.findall(r'&(\w)', s.split('\t')[0])


def main():
    check = '--check' in sys.argv
    code_keys = keys_in_code()
    langs, bad = [], 0
    for po in sorted(glob.glob(os.path.join(ROOT, 'lang', '*.po'))):
        header, table = read_po(po)
        code = header.get('Language') or os.path.basename(po)[:-3]
        native = header.get('X-Native-Name') or code
        missing = [k for k in code_keys if not table.get(k)]
        orphans = [k for k in table if k not in code_keys]
        print('%s (%s): %d texts in the code, %d translated, %d missing, %d no longer used'
              % (code, native, len(code_keys), len(code_keys) - len(missing), len(missing), len(orphans)))
        for k in missing:
            print('  missing: %-70r %s' % (k[:70], code_keys[k][0]))
        for k in orphans:
            print('  unused:  %r' % k[:70])
        for k, v in table.items():
            if v and specs(k) != specs(v):
                print('  FORMAT MISMATCH: %r -> %r' % (k[:60], v[:60])); bad += 1
            if v and k.count('\t') != v.count('\t'):
                print('  TAB MISMATCH: %r -> %r' % (k[:60], v[:60])); bad += 1
            if v and len(accel(k)) != len(accel(v)):
                print('  MNEMONIC (&) MISMATCH: %r -> %r' % (k[:60], v[:60])); bad += 1
        langs.append((code, native, {k: v for k, v in table.items() if v and k in code_keys}))
    if check:
        sys.exit(1 if bad else 0)

    out = ['/* GENERATED by tools/lang_to_c.py from the .po files in lang/. Do not edit: change the .po file and run the tool. */',
           '#include "lang_tables.h"', '']
    names = []
    for code, native, table in langs:
        if code == 'en':
            continue
        ident = re.sub(r'\W', '_', code)
        out.append('static const LangEntry lang_%s[] = {' % ident)
        for k in sorted(table):
            out.append('    { %s,\n      %s },' % (c_encode(k), c_encode(table[k])))
        out.append('};')
        out.append('')
        names.append((code, native, ident, len(table)))
    out.append('const LangTable g_lang_tables[] = {')
    out.append('    { "en", "English", 0, 0 },')
    for code, native, ident, n in names:
        out.append('    { %s, %s, lang_%s, %d },' % (c_encode(code), c_encode(native), ident, n))
    out.append('};')
    out.append('const int g_lang_table_count = %d;' % (1 + len(names)))
    out.append('')
    with open(os.path.join(ROOT, 'runtime', 'lang_tables.c'), 'w', encoding='utf-8', newline='\n') as f:
        f.write('\n'.join(out))
    print('wrote runtime/lang_tables.c (%d languages besides English)' % len(names))
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
