"""Task 194: the game's gzip (telemetry_logic.h) read back by Python's own gzip, and what a crash report's
upload body holds after redaction. Runs after kk-telemetry-tests, which writes the files.
Usage: telemetry_gzip_tests.py <out folder of kk-telemetry-tests>"""
import gzip
import json
import sys
from pathlib import Path

out = Path(sys.argv[1])
failures = []

samples = sorted(out.glob('*.orig'))
if len(samples) < 5:
    failures.append(f'only {len(samples)} gzip samples in {out}')
for orig in samples:
    gz = orig.with_suffix('.gz')
    try:
        data = gzip.decompress(gz.read_bytes())
    except Exception as e:  # noqa: BLE001
        failures.append(f'{gz.name}: not readable gzip: {e}')
        continue
    if data != orig.read_bytes():
        failures.append(f'{gz.name}: decompresses to different bytes ({len(data)} vs {orig.stat().st_size})')

body = (out / 'crash-body.gz').read_bytes()
text = gzip.decompress(body).decode('utf-8')
if 'TekRa' in text or 'tekra' in text.lower():
    failures.append('the crash upload still holds the user name')
if '%USERPROFILE%\\Documents\\king_kong\\x.log' not in text:
    failures.append('the crash upload lost its redacted path')
if 'king_kong.exe + 0x1234' not in text or 'Version: 2.0.0-beta.7' not in text:
    failures.append('the crash upload lost the stack or version')

perf = json.loads((out / 'perf.json').read_text(encoding='utf-8'))
for key in ('installId', 'version', 'api', 'settings', 'chapters', 'pipelineLibrary', 'startupFallback'):
    if key not in perf:
        failures.append(f'the summary has no {key}')
if not 0 < len(perf.get('chapters', [])) <= 64:
    failures.append('the summary has the wrong number of chapters')

# Task 211: the summary with worst-frame detail is valid JSON with the fields the server keeps.
worst = json.loads((out / 'perf-worst.json').read_text(encoding='utf-8'))
frames = [f for c in worst.get('chapters', []) for f in c.get('worst', [])]
if not frames:
    failures.append('the worst-frame summary has no detail')
for f in frames:
    for key in ('ms', 'phase', 'cpuMs', 'swapMs', 'gpuMs', 'pipeWaitMs', 'texUploads', 'reads', 'focus', 'paused', 'cap'):
        if key not in f:
            failures.append(f'a worst frame has no {key}')
    if f.get('phase') not in ('play', 'load'):
        failures.append(f'a worst frame has the phase {f.get("phase")!r}')
    if any(isinstance(v, str) and ('\\' in v or '/' in v) for v in f.values()):
        failures.append('a worst frame holds a path')

if failures:
    print('\n'.join('FAIL: ' + f for f in failures))
    sys.exit(1)
print(f'{len(samples)} gzip samples read back by Python; crash upload redacted; summary is valid JSON')
