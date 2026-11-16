"""Read all registered mmregdb domains; preserve every per-channel driver field."""
import argparse
import json
import re
from pathlib import Path

HERE = Path(__file__).resolve().parent
DRIVER_PATH = Path('firmware/platforms/esp-halow/halow/components/mm-iot-sdk/framework/src/mmregdb/mmregdb.c')


def find_source():
    for parent in HERE.parents:
        for candidate in (parent / DRIVER_PATH, parent / '01_Software_Source' / DRIVER_PATH):
            if candidate.is_file():
                return candidate
    return None


SOURCE = find_source()
OUTPUT = HERE / 'radio-catalog.js'
KEYS = ['frequencyHz', 'dutyCycleCentiPercent', 'omitControlResponse',
        'globalOperatingClass', 's1gOperatingClass', 'number', 'bandwidthMHz',
        'maxEirpDbm', 'minPacketSpacingUs', 'airtimeMinUs', 'airtimeMaxUs']


def generate():
    raw = SOURCE.read_text(encoding='utf-8')
    arrays = {}
    for name, block in re.findall(r'const struct mmwlan_s1g_channel (\w+)\[\] = \{(.*?)\n\};', raw, re.S):
        rows = []
        for row in re.findall(r'\{([^{}]+)\}', block):
            values = [v.strip() for v in row.split(',')]
            if len(values) != len(KEYS):
                raise ValueError(f'Unsupported driver row in {name}: {row}')
            item = {key: value == 'true' if key == 'omitControlResponse' else int(value)
                    for key, value in zip(KEYS, values)}
            item['frequencyMHz'] = item['frequencyHz'] / 1_000_000
            rows.append(item)
        arrays[name] = rows
    lists = {}
    for name, block in re.findall(r'const struct mmwlan_s1g_channel_list (\w+) = \{(.*?)\n\};', raw, re.S):
        code = re.search(r'country_code = "(\w+)"', block).group(1)
        pointer = re.search(r'\.channels = (&?)(\w+)(?:\[(\d+)\])?', block)
        offset = int(pointer.group(3) or 0)
        rows = arrays[pointer.group(2)]
        count = re.search(r'num_channels = (\d+)', block)
        if not count and not re.search(r'num_channels = \(sizeof\(', block):
            raise ValueError(f'Unknown channel-count expression: {name}')
        lists[name] = (code, rows[offset:offset + int(count.group(1))] if count else rows)
    domain_block = re.search(r'regulatory_db_domains\[\] = \{(.*?)\};', raw, re.S).group(1)
    catalog = {}
    for name in re.findall(r'&(\w+)', domain_block):
        if name not in lists:
            macro = re.search(r'#define\s+' + re.escape(name) + r'\s+(\w+)', raw)
            if not macro:
                raise ValueError(f'Unresolved registered domain: {name}')
            name = macro.group(1)
        code, rows = lists[name]
        if code in catalog or not rows:
            raise ValueError(f'Duplicate or empty domain: {code}')
        catalog[code] = rows
    header = ('// Generated from bundled mmregdb.c; AU uses its default REVMF domain.\n'
              '// All 11 per-channel driver fields retained. Template data is not hardware certification.\n')
    return header + 'window.EdgezRadioCatalog = ' + json.dumps(catalog, ensure_ascii=False, indent=2) + ';\n', catalog


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--check', action='store_true', help='Check without writing')
    parser.add_argument('--source', type=Path, help='Explicit path to the pinned mmregdb.c source')
    args = parser.parse_args()
    SOURCE = args.source if args.source is not None else SOURCE
    if SOURCE is None or not SOURCE.is_file():
        parser.error('Driver source not found. Initialize the pinned firmware dependencies or pass --source /path/to/mmregdb.c. The bundled catalog remains usable for viewing the UI.')
    generated, catalog = generate()
    if args.check:
        if OUTPUT.read_text(encoding='utf-8') != generated:
            raise SystemExit('Channel catalog differs from the registered firmware template domains.')
    else:
        OUTPUT.write_text(generated, encoding='utf-8')
    print(json.dumps({'mode': 'checked' if args.check else 'generated',
                      'countries': {code: len(rows) for code, rows in catalog.items()},
                      'channelRecords': sum(map(len, catalog.values()))}))
