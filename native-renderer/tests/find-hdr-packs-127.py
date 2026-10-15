"""Read only: find prepared packs matching this branch's translator."""
from pathlib import Path
import struct
import xxhash
root=Path(__file__).resolve().parents[2]
prelude=(root/'native-renderer/shaders/hlsl/kk_common.hlsli').read_bytes()
wanted=xxhash.xxh3_64_intdigest(struct.pack('<III',5,2,len(prelude))+prelude)
print(f'wanted translator {wanted:016X}')
found=[]
for folder in (Path(r'F:\KK-native-renderer\runs'),Path(r'F:\KK-native-renderer\testbuilds'),Path(r'F:\KK-native-renderer\wt')):
    for child in folder.iterdir():
        if not child.is_dir(): continue
        for sub in ('','cache/native-shaders','user/cache/native-shaders','KingKong/user/cache/native-shaders','kk/out/build/kk-dev-vulkan'):
            base=child/sub
            if not base.is_dir():continue
            for pack in base.glob('*.pack'):
                with pack.open('rb') as f: header=f.read(16)
                if len(header)==16 and struct.unpack_from('<Q',header,8)[0]==wanted:
                    found.append((str(pack),pack.stat().st_size))
for path,size in found:print(size,path)
