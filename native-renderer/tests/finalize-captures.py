"""Finalize legacy dev captures after exit; preserve originals under moved-aside.
New Windows builds write PNG directly and require no conversion. No game is launched.
"""
from pathlib import Path
import argparse
import io
import json
import os
import sys
import time
import numpy as np
from PIL import Image
sys.path.insert(0, str(Path(__file__).resolve().parent))
from compare import load_image


def finalize(run, archive_root):
    run, archive_root = Path(run).resolve(), Path(archive_root).resolve()
    if not run.is_dir() or archive_root == run or run in archive_root.parents:
        raise ValueError('Archive must be outside the run directory')
    archived = archive_root / (run.name + '-' + str(time.time_ns()))
    before = after = count = 0
    # Only raw captures beside the game. Never touch private userdata or launcher art.
    for source in sorted(run.glob('*.bmp')):
        if source.is_symlink():
            raise ValueError('Refusing a symlink capture')
        rgb = load_image(source)
        png = source.with_suffix('.png')
        if png.exists():
            if not np.array_equal(rgb, load_image(png)):
                raise ValueError(f'Conflicting PNG: {png}')
        else:
            buffer = io.BytesIO()
            Image.fromarray(rgb).save(buffer, format='PNG')
            encoded = buffer.getvalue()
            with Image.open(io.BytesIO(encoded)) as check:
                if not np.array_equal(rgb, np.asarray(check.convert('RGB'))):
                    raise ValueError(f'PNG verification failed: {source}')
            with png.open('xb') as out:
                out.write(encoded)
                out.flush()
                os.fsync(out.fileno())
            if not np.array_equal(rgb, load_image(png)):
                raise ValueError(f'Written PNG verification failed: {png}')
        before += source.stat().st_size
        after += png.stat().st_size
        archived.mkdir(parents=True, exist_ok=True)
        source.rename(archived / source.name)
        count += 1
    return dict(converted=count, bmp_bytes=before, png_bytes=after,
                archived_originals=str(archived) if count else None)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=Path)
    parser.add_argument('--archive-root', type=Path, default=Path(r'F:\KK-native-renderer\moved-aside\task029-legacy-captures'))
    args = parser.parse_args()
    print(json.dumps(finalize(args.run, args.archive_root)))
