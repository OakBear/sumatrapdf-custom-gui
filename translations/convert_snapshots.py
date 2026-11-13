#!/usr/bin/env python3
"""Convert saved public AppTranslator pages to a local SumatraPDF cache.

Input: apptranslator-SumatraPDF-{cn,tw}-snapshot.html in this directory.
The second output line is a locally computed body SHA-1, not a server token.
No network requests are made. See the adjacent source snapshots for attribution.
"""
from collections import Counter
from hashlib import sha1, sha256
from html.parser import HTMLParser
from pathlib import Path
import json
import re

ROOT = Path(__file__).resolve().parent
OUT = ROOT.parent / 'tree/.work/translations.txt'
LANGS = ('cn', 'tw')

class Page(HTMLParser):
    def __init__(self):
        super().__init__(convert_charrefs=True)
        self.rows = []
        self.row = None
        self.field = None
        self.depth = 0

    def handle_starttag(self, tag, attrs):
        attrs = dict(attrs)
        classes = attrs.get('class', '').split()
        if tag == 'div' and 'trans' in classes:
            if self.row is not None:
                raise ValueError('nested translation row')
            self.row = {'attr': attrs.get('data-string'), 'orig': None, 'trans': None}
            self.depth = 1
        elif self.row is not None:
            if tag == 'div':
                self.depth += 1
            if self.field is not None:
                raise ValueError('nested markup in string field')
            if tag == 'span' and ('origstr' in classes or 'transstr' in classes):
                self.field = 'orig' if 'origstr' in classes else 'trans'
                if self.row[self.field] is not None:
                    raise ValueError('duplicate string field')
                self.row[self.field] = ''

    def handle_endtag(self, tag):
        if self.row is None:
            return
        if self.field is not None and tag == 'span':
            self.field = None
        if tag == 'div':
            self.depth -= 1
            if self.depth == 0:
                self.rows.append(self.row)
                self.row = None

    def handle_data(self, data):
        if self.field is not None:
            self.row[self.field] += data

# Mirrors UnescapeTemp on UTF-8 bytes, including unknown escapes.
def unescape(text):
    src = text.encode('utf-8'); out = bytearray(); i = 0
    escapes = {ord('t'): 9, ord('n'): 10, ord('r'): 13,
               ord('\\'): 92, ord('"'): 34, ord("'"): 39}
    while i < len(src):
        c = src[i]; i += 1
        if c != 92 or i == len(src):
            out.append(c); continue
        c2 = src[i]; i += 1
        if c2 in escapes:
            out.append(escapes[c2])
        elif c2 == ord('x') and i + 1 < len(src) and re.fullmatch(b'[0-9a-fA-F]{2}', src[i:i+2]):
            out.append(int(src[i:i+2], 16)); i += 2
        else:
            out.extend((c, c2))
    return bytes(out)

def parse_page(lang):
    path = ROOT / ('apptranslator-SumatraPDF-' + lang + '-snapshot.html')
    raw = path.read_bytes()
    page = Page(); page.feed(raw.decode('utf-8')); page.close()
    if page.row is not None or not page.rows:
        raise ValueError('incomplete or empty page: ' + lang)
    rows = {}
    for row in page.rows:
        source, text = row['orig'], row['trans']
        if source is None or row['attr'] != source:
            raise ValueError('source attribute and displayed text disagree')
        if not source or text is None:
            raise ValueError('missing source or translation field')
        if any(c in source or c in text for c in ('\n', '\r', '\x00')):
            raise ValueError('physical line break or NUL cannot be represented')
        if source in rows:
            raise ValueError('duplicate source: ' + source)
        rows[source] = text
    return rows, sha256(raw).hexdigest()

def runtime_parse(data, lang):
    # Match ParseTranslationsTxt: skip two lines, take first lang value per source.
    lines = data.splitlines(); pairs = {}; i = 2
    while i < len(lines):
        line = lines[i]
        if not line.startswith(':'):
            raise ValueError('invalid source line')
        source = line[1:]; i += 1; text = ''
        while i < len(lines) and not lines[i].startswith(':'):
            if not text and lines[i].startswith(lang + ':'):
                text = lines[i][len(lang)+1:]
            i += 1
        key = unescape(source)
        if key in pairs:
            raise ValueError('duplicate source after runtime unescaping')
        pairs[key] = unescape(text)
    return pairs

def main():
    assert unescape(r'a\n\t\xC2\xB7\"\\\q') == b'a\n\t\xc2\xb7"\\\\q'
    snapshots = {}; hashes = {}
    for lang in LANGS:
        snapshots[lang], hashes[lang] = parse_page(lang)
    # The public cn snapshot reverses the meaning of the two %d arguments.
    # Keep the unmodified snapshot as provenance and correct the local output.
    key = '(the first %d of %d characters; Copy gets all of them)'
    original = '（%d 个字符中的前 %d 个；复制可获取全部）'
    if snapshots['cn'].get(key) != original:
        raise ValueError('expected Chinese translation for local correction changed')
    snapshots['cn'][key] = '（前 %d 个字符，共 %d 个；复制可获取全部）'
    key = 'Bookmark shortcut to page %s of %s'
    for lang, original, corrected in (
        ('cn', '书签快捷方式：第 %s 页/共 %s 页', '书签快捷方式：第 %s 页，文件 %s'),
        ('tw', '書籤捷徑指向第 %s 頁 (共 %s 頁)', '書籤捷徑：第 %s 頁，檔案 %s'),
    ):
        if snapshots[lang].get(key) != original:
            raise ValueError('expected bookmark translation for local correction changed: ' + lang)
        snapshots[lang][key] = corrected
    keys = sorted(set().union(*(set(v) for v in snapshots.values())))
    lines = []
    for key in keys:
        lines.append(':' + key)
        for lang in LANGS:
            val = snapshots[lang].get(key)
            if val:
                lines.append(lang + ':' + val)
    body = '\n'.join(lines) + '\n'
    digest = sha1(body.encode('utf-8')).hexdigest()
    output = 'AppTranslator: SumatraPDF\n' + digest + '\n' + body
    for lang in LANGS:
        parsed = runtime_parse(output, lang)
        expected = {unescape(k): unescape(snapshots[lang].get(k, '')) for k in keys}
        if len(expected) != len(keys) or parsed != expected:
            raise ValueError('runtime parser round-trip mismatch: ' + lang)
    OUT.parent.mkdir(parents=True, exist_ok=True)
    with OUT.open('x', encoding='utf-8', newline='') as f:
        f.write(output)
    print(json.dumps({'output': str(OUT), 'output_sha256': sha256(output.encode()).hexdigest(),
        'synthetic_body_sha1': digest, 'source_sha256': hashes, 'unique_sources': len(keys),
        'translated': {lang: sum(bool(v) for v in snapshots[lang].values()) for lang in LANGS}}, indent=2))

if __name__ == '__main__':
    main()
