#!/usr/bin/env python3
"""Build wfc/YDQE/dqvc.txt: DQVC's 52 weekly stocks on the original calendar.

  dqvc_build.py <saved pages dir> <items.json> <out dqvc.txt> [archived auction files...]

Saved pages: yab's DQ9 Tools (yabd.org/apps/dq9), the DQVC Listing page (the
52 sales and the dates they started each year) and the 52 DQVC Item List
pages (dqvcitems.php?week=N, each week's items and prices), as saved by a
browser; any file names (the page's own address is read from the file).
items.json: {item id: [name, type]} (from the open-source DQIX save editor).
Quantities are not on the pages: the archived files' quantities are used
where an item appears in one, otherwise 1 for equipment and 3 for others.
"""
import difflib, html, json, os, re, sys, struct, datetime

def cells(row):
    return [html.unescape(re.sub(r'<[^>]+>', '', c)).strip() for c in re.findall(r'(?s)<t[dh][^>]*>(.*?)</t[dh]>', row)]

def pages(d):
    for f in sorted(os.listdir(d)):
        if f.endswith('.html') or f.endswith('.htm'):
            s = open(os.path.join(d, f), encoding='utf-8', errors='replace').read()
            m = re.search(r'saved from url=\(\d+\)(\S+)', s)
            w = re.search(r'week_?(\d+)\.html?$', f)                 # downloaded as weekN.html
            url = m.group(1) if m else (f'dqvcitems.php?week={w.group(1)}' if w else '')
            yield url, s

def rc4(key, data):
    S = list(range(256)); j = 0
    for i in range(256):
        j = (j + S[i] + key[i % len(key)]) & 255; S[i], S[j] = S[j], S[i]
    i = j = 0; out = bytearray()
    for b in data:
        i = (i + 1) & 255; j = (j + S[i]) & 255; S[i], S[j] = S[j], S[i]; out.append(b ^ S[(S[i] + S[j]) & 255])
    return bytes(out)

def archived_qty(files):
    q = {}
    for f in files:
        p = rc4(b'XENLONPROJECTKEY', open(f, 'rb').read()[:-4])
        end = struct.unpack('<I', p[4:8])[0]; o = 16
        while o < end:
            tag = p[o] | p[o + 1] << 8; n = p[o + 2]
            if tag == 0x68:
                i, pr, _, qty = struct.unpack('<IIII', p[o + 4:o + 20]); q[i] = max(q.get(i, 0), qty)
            o += 4 + 4 * n
    return q

def main():
    pdir, items_json, out = sys.argv[1:4]
    items = {int(k): v for k, v in json.load(open(items_json)).items()}
    by_name = {}
    for i, (n, t) in items.items():
        by_name.setdefault(n.lower(), (i, t))
    qty = archived_qty(sys.argv[4:])
    calendar, lists = {}, {}
    for url, s in pages(pdir):
        s = re.sub(r'(?s)<script.*?</script>|<style.*?</style>', '', s)
        if 'dqvclist.php' in url or 'DQVC Listing' in s[:2000]:
            for r in re.findall(r'(?s)<tr.*?</tr>', s):
                w = re.search(r'week=(\d+)', r)
                if w: calendar[int(w.group(1))] = cells(r)
        m = re.search(r'dqvcitems\.php\?week=(\d+)', url)
        if m:
            title = re.search(r'DQVC Item List\s*</[^>]+>\s*(?:<[^>]+>\s*)*([^<]+)', s)
            rows = [c for c in (cells(r) for r in re.findall(r'(?s)<tr.*?</tr>', s)) if len(c) == 3 and c[0].isdigit()]
            lists[int(m.group(1))] = rows
    lines = ['# DQVC: the 52 weekly sales, on the calendar the original service used every',
             '# year (July 2010 - 2014). Made by tools/dqvc_build.py from yab\'s DQ9 Tools.',
             '#   week MM/DD <sale>   (the date it started in 2011-12)',
             '#   <item id> <price> <quantity> <name>',
             '# The game shows six items of the week, picked per player.', '']
    missing, unknown = [], []
    for w in sorted(calendar):
        c = calendar[w]
        d = datetime.datetime.strptime(c[1], '%b %d, %Y')
        lines.append(f'week {d.month:02d}/{d.day:02d} {c[-1]}')
        if w not in lists: missing.append(w); lines.append('')
        else:
            for _, name, price in lists[w]:
                hit = by_name.get(name.lower()) or by_name.get(name.lower().replace("'", "’"))
                if not hit:                                         # a spelling difference (e.g. "sandles")
                    close = difflib.get_close_matches(name.lower(), list(by_name), n=1, cutoff=0.9)
                    hit = by_name[close[0]] if close else None
                    if hit: print(f'  week {w}: "{name}" taken as "{close[0]}"')
                if not hit: unknown.append((w, name)); continue
                i, t = hit
                q = qty.get(i) or (1 if t not in ('ITEM_TYPE_COMMON',) else 3)
                lines.append(f'{i} {int(price.replace(",", ""))} {q} {name}')
            lines.append('')
    open(out, 'w', newline='\n').write('\n'.join(lines))
    print(f'{len(calendar)} weeks, {len(lists)} item lists; missing {missing}')
    for w, n in unknown: print(f'  week {w}: no item called "{n}"')

main()
