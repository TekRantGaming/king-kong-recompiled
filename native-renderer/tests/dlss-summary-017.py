"""Summarize task 017 private runs and retain every spike's observed counters."""
import csv
import json
import statistics
import sys
from pathlib import Path


def rows(path):
    if not path.exists():
        return []
    with path.open(encoding='utf-8-sig', errors='replace') as stream:
        result = []
        for row in csv.DictReader(stream):
            try:
                result.append({key: float(value) for key, value in row.items()})
            except (ValueError, TypeError):
                pass  # an unfinished final row of an active run
        return result


def percentile(values, fraction):
    return sorted(values)[int((len(values) - 1) * fraction)] if values else None


def summarize(run):
    config = json.loads((run / 'test.json').read_text(encoding='utf-8-sig'))
    host = [row for row in rows(run / 'frame_times.csv') if 0 <= row['t'] <= 120 and row['host_ms'] > 0]
    native = rows(run / 'native-frames.csv')
    # The clocks share steady_clock's epoch. Include only frames that ended during the loaded
    # level's logged window; the first interval may include the load, so leave it out.
    if host and 'wall_s' in host[0]:
        native = [row for row in native if host[0]['wall_s'] < row['wall_s'] <= host[-1]['wall_s']]
    else:
        native = []
    host_median = statistics.median(row['host_ms'] for row in host) if host else None
    native_median = statistics.median(row['interval_ms'] for row in native) if native else None
    host_spikes = [row for row in host if row['host_ms'] > 2 * host_median] if host else []
    spikes = [row.copy() for row in native if row['interval_ms'] > 2 * native_median] if native else []
    timed = {}
    for name in ('sdk', 'motion', 'upscale'):
        timed[name] = {int(row['frame']): row['gpu_ms'] for row in rows(run / f'native-frames.csv.{name}-gpu.csv')}
        for row in spikes:
            row[name + '_gpu_ms'] = timed[name].get(int(row['frame']), '')
    for row in spikes:
        # These are observed contributors, not exclusive causal diagnoses. Motion CPU time
        # includes waits in replay pipeline creation, and SDK CPU time can include feature setup.
        row['observations'] = ', '.join(name for name, condition in (
            ('pipeline wait', row['pipeline_wait_ms'] >= 1),
            ('motion CPU', row['motion_cpu_ms'] >= 2),
            ('SDK CPU', row['sdk_cpu_ms'] >= 2),
            ('resource creation', row['resource_creations'] > 0),
            ('target CPU', row['target_cpu_ms'] >= 2),
        ) if condition) or 'no measured CPU contributor above thresholds'
    if spikes:
        with (run / 'spikes-017.csv').open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(spikes[0]))
            writer.writeheader()
            writer.writerows(spikes)
    if host_spikes:
        with (run / 'host-spikes-017.csv').open('w', newline='', encoding='utf-8') as stream:
            writer = csv.DictWriter(stream, fieldnames=list(host_spikes[0]))
            writer.writeheader()
            writer.writerows(host_spikes)
    log = '\n'.join(path.read_text(encoding='utf-8', errors='replace') for path in run.glob('run*.log'))
    result = dict(path=str(run), label=config['label'], chapter=config['chapter'], backend=config['backend'], scale=config['scale'],
                  dll_sha256=config['dll_sha256'], host_samples=len(host), host_median_ms=host_median,
                  host_spikes=len(host_spikes), host_max_ms=max((row['host_ms'] for row in host), default=None),
                  native_samples=len(native), native_median_ms=native_median, native_spikes=len(spikes),
                  native_max_ms=max((row['interval_ms'] for row in native), default=None),
                  pipeline_spikes=sum(row['pipeline_wait_ms'] >= 1 for row in spikes),
                  motion_cpu_spikes=sum(row['motion_cpu_ms'] >= 2 for row in spikes),
                  sdk_cpu_spikes=sum(row['sdk_cpu_ms'] >= 2 for row in spikes),
                  resource_spikes=sum(row['resource_creations'] > 0 for row in spikes),
                  pipeline_wait_ms=sum(row['pipeline_wait_ms'] for row in native),
                  wait_frames=sum(row['pipeline_wait_ms'] > 0 for row in native),
                  skipped_draws=sum(row['skipped_draws'] for row in native),
                  exhaustion_errors=sum('insufficient' in line.lower() and ('maxversions' in line.lower() or 'query pool' in line.lower()) for line in log.splitlines()),
                  shots=len(list(run.glob('shot_*.bmp'))),
                  errors=[line for line in log.splitlines() if 'Device Removed' in line or 'NVRHI: ERROR' in line],
                  upscaler=[line for line in log.splitlines() if 'upscaler DLSS (asked:' in line],
                  list_status=[line for line in log.splitlines() if 'pipeline list ' in line and 'descriptions' in line],
                  last_stutter=[line for line in log.splitlines() if 'pipeline stutter' in line][-1:])
    for name, samples in timed.items():
        selected = [samples[int(row['frame'])] for row in native if int(row['frame']) in samples]
        result[name + '_gpu_median_ms'] = percentile(selected, .5)
        result[name + '_gpu_p95_ms'] = percentile(selected, .95)
    # Expose a first-20-second cutscene subset separately from its subsequent gameplay.
    if 'cutscene' in config['label']:
        early = [row for row in host if row['t'] <= 20]
        result['cutscene_host_samples'] = len(early)
        result['cutscene_host_median_ms'] = statistics.median(row['host_ms'] for row in early) if early else None
        result['cutscene_host_spikes'] = sum(row['host_ms'] > 2 * result['cutscene_host_median_ms'] for row in early)
    return result


if __name__ == '__main__':
    roots = [Path(value) for value in sys.argv[1:]]
    runs = [run for root in roots for run in (root.glob('task017-*') if root.is_dir() and not (root / 'test.json').exists() else [root]) if (run / 'test.json').exists()]
    print(json.dumps([summarize(run) for run in sorted(set(runs))], indent=2))
