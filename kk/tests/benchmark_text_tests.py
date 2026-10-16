"""Every benchmark UI label/template, including dynamic metrics, has five languages."""
from pathlib import Path
import re
import sys
root = Path(sys.argv[1])
table = '\n'.join((root / 'src' / f).read_text(encoding='utf-8') for f in
                  ('launcher_text_table.inc', 'debug_overlay_text_table.inc', 'benchmark_text_table.inc'))
literal = r'"((?:[^"\\]|\\.)*)"'
entries = {}
for match in re.finditer(r'\{\s*' + literal + (r'\s*,\s*' + literal) * 4 + r'\s*\}', table):
    entries.setdefault(match[1], match.groups())
source = (root / 'src/benchmark.cpp').read_text(encoding='utf-8')
keys = set(re.findall(r'\b(?:T|Heading|row|F)\("([^"\n]*)"', source))
# The launcher's benchmark pages: every UI literal (adjacent literals joined).
pages = (root / 'src/launcher_benchmark.cpp').read_text(encoding='utf-8')
pages = re.sub(r'"\s*\n\s*"', '', pages)
keys.update(k for k in re.findall(literal, pages) if re.match(r'[A-Z{]', k) and not k.startswith('### '))
results = (root / 'src/benchmark_results.h').read_text(encoding='utf-8')
keys.update(re.findall(r'return "([^"\n]+)"', results[results.index('ConfidenceLabel'):]))
keys.add('Benchmark results copied to the clipboard.')
assert 'Standard run' in keys and 'Whole route' in keys and 'High' in keys, 'page keys not collected'
metrics = (root / 'src/benchmark_monitor.h').read_text(encoding='utf-8')
metric_names = metrics[metrics.index('kMetricNames[]'):metrics.index('using Values')]
keys.update(re.findall(literal, metric_names))
keys.update(re.findall(r'return "([^"\n]+)"', metrics))
keys.update(['Benchmark', 'Benchmark results', 'Chapter', 'Total', 'Custom', 'Off', 'Previous',
             'Recording route', 'Live performance'])
missing = sorted(keys - entries.keys())
assert not missing, 'Missing translations: ' + repr(missing)
for key in keys:
    row = entries[key]
    tokens = re.findall(r'\{\d+\}|%(?:\.\d+)?[suf]', row[0])
    for value in row:
        assert value and '\u2014' not in value, key
        assert re.findall(r'\{\d+\}|%(?:\.\d+)?[suf]', value) == tokens, key
print(f'{len(keys)} benchmark keys: all five languages, placeholders and text rules PASS')
