"""Compile complete offline language packs and validate all interpolation parameters."""
import argparse
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
CODES = ['zh-Hant', 'fr', 'de', 'es', 'it', 'pt', 'nl', 'ja', 'ko', 'hi']


def compile_packs():
    rows = json.loads((HERE / 'messages.json').read_text(encoding='utf-8'))
    expected = {row['id'] for row in rows}
    packs = {}
    for code in CODES:
        lines = (HERE / (code + '.txt')).read_text(encoding='utf-8').splitlines()
        if code == 'zh-Hant':
            assert len(lines) == len(rows), (code, len(lines))
            values = dict(zip([row['id'] for row in rows], lines))
        else:
            values = {}
            for line in lines:
                if not line.strip():
                    continue
                key, value = line.split('=', 1)
                assert key not in values, (code, 'duplicate', key)
                values[key] = value
        assert set(values) == expected, (code, 'missing', expected - set(values), 'extra', set(values) - expected)
        for row in rows:
            text = values[row['id']]
            assert text.strip(), (code, row['id'], 'empty')
            assert set(re.findall(r'\{p\d+\}', text)) == set(re.findall(r'\{p\d+\}', row['en'])), (code, row['id'], 'parameters')
        packs[code] = [values[row['id']] for row in rows]
    base = 'window.EdgezLocaleData = ' + json.dumps({code: [row[code] for row in rows] for code in ['en', 'zh']}, ensure_ascii=False, separators=(',', ':')) + ';\n'
    extra = '/* Complete offline packs; order follows messages.json. No runtime translation service. */\nObject.assign(window.EdgezLocaleData,' + json.dumps(packs, ensure_ascii=False, separators=(',', ':')) + ');\n'
    return base, extra, len(rows)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true')
    args = parser.parse_args()
    base, extra, count = compile_packs()
    for filename, content in [('base.js', base), ('packs.js', extra)]:
        if args.check:
            assert (HERE / filename).read_text(encoding='utf-8') == content, filename + ' needs regeneration'
        else:
            (HERE / filename).write_text(content, encoding='utf-8')
    print(json.dumps({'languages': 12, 'messagesPerLanguage': count, 'completeTranslations': count * 12, 'mode': 'checked' if args.check else 'compiled'}))
