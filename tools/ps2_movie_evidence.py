"""Private task-118 evidence. Outputs game-derived data only to --output, never the repo.

Run with the absolute Python312 interpreter and -I. Requires the existing VS
developer environment for --scan; --frames uses the already installed ffmpeg.
"""
import argparse
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parents[1]
SCANNER = r'''
int main(int argc, char** argv) {
  if (argc != 3) return 2;
  const std::filesystem::path game = argv[1], dest = argv[2];
  std::filesystem::create_directories(dest);
  std::ofstream log(dest / "data-scan.txt");
  for (const char* name : {"KKMaps.bf", "KKTextures.bf"}) {
    std::ifstream f(game / name, std::ios::binary);
    f.seekg(0, std::ios::end); uint64_t size = f.tellg();
    std::vector<std::pair<uint32_t,uint32_t>> entries;
    uint64_t table = 0x2c;
    for (int n=0; n<64 && table+24<=size; ++n) {
      uint8_t h[24]; f.seekg(table); f.read((char*)h,24);
      uint32_t count=LE32(h), start=LE32(h+8), next=LE32(h+12);
      if (count > 1048576 || start+uint64_t(count)*8>size) return 3;
      std::vector<uint8_t> e(size_t(count)*8); f.seekg(start); f.read((char*)e.data(),e.size());
      for (size_t i=0;i<count;++i) entries.emplace_back(LE32(&e[i*8]),LE32(&e[i*8+4]));
      if (next==0xffffffff || next<24) break; table=next-24;
    }
    log << name << " entries " << entries.size() << "\n";
    size_t total=0, bad=0;
    for (auto [offset,key] : entries) {
      std::vector<uint8_t> data;
      uint8_t h[12]; f.seekg(offset); f.read((char*)h,12);
      uint64_t end=std::min<uint64_t>(offset+4ull+LE32(h),size);
      for(uint64_t pos=offset;pos+12<=end;) {
        f.seekg(pos); if(!f.read((char*)h,12)) break;
        uint32_t plain=LE32(h+4), stored=LE32(h+8);
        if(!plain || plain>(1u<<24) || stored>plain || pos+12+stored>end) break;
        std::vector<uint8_t> buf(stored); f.read((char*)buf.data(),stored);
        size_t at=data.size();data.resize(at+plain);
        if(plain==stored) std::memcpy(data.data()+at,buf.data(),plain);
        else if(!Lzo1xDecompress(buf.data(),stored,data.data()+at,plain)) {++bad;data.resize(at);break;}
        pos+=12+stored;
      }
      total+=data.size();
      bool hit=false;
      for(size_t i=0;i+3<data.size();++i) {
        bool bk=data[i]=='B' && data[i+1]=='K' && data[i+2]=='_';
        bool movie=i+5<data.size() && (data[i]=='m'||data[i]=='M') && data[i+1]=='o' && data[i+2]=='v' && data[i+3]=='i' && data[i+4]=='e';
        if(!bk && !movie) continue;
        size_t a=i;while(a>0 && i-a<80 && data[a-1]>=32 && data[a-1]<127)--a;
        size_t b=i;while(b<data.size() && b-i<160 && data[b]>=32 && data[b]<127)++b;
        log << std::hex << key << " +" << i << " " << std::string((char*)data.data()+a,b-a) << "\n";
        hit=true; i=b>i?b-1:i;
      }
      if(hit || key==0x3d00c45a || key==0xfd10c45a || key==0xff80c45a) {
        char file[100];std::snprintf(file,sizeof(file),"%s-%08X.bin",name,key);
        std::ofstream out(dest/file,std::ios::binary);out.write((char*)data.data(),data.size());
      }
    }
    log << std::dec << "decompressed bytes " << total << " bad chunks " << bad << "\n";
    std::cout << name << " " << entries.size() << " entries, " << total << " bytes, bad " << bad << "\n";
  }
}
'''


def scan(args):
    source = (ROOT / 'kk/src/launcher_art.cpp').read_text()
    helpers = source[source.index('bool Lzo1xDecompress('):source.index('// Xenos tiled layout')]
    prefix = '\n'.join('#include <' + h + '>' for h in
                       ['algorithm', 'cstdint', 'cstring', 'fstream', 'filesystem', 'vector', 'string', 'cstdio', 'iostream'])
    cpp = args.output / 'scan.cpp'
    cpp.write_text(prefix + '\n' + helpers + SCANNER)
    exe = args.output / 'scan.exe'
    subprocess.run(['cl.exe', '/nologo', '/O2', '/EHsc', '/std:c++20', str(cpp),
                    '/Fe:' + str(exe), '/Fo:' + str(args.output / 'scan.obj')], check=True)
    subprocess.run([str(exe), str(args.game), str(args.output)], check=True)


def frames(args):
    from PIL import Image, ImageDraw
    records = []
    for movie in sorted(args.movies.glob('*.mp4')):
        probe = json.loads(subprocess.check_output([str(args.ffmpeg.with_name('ffprobe.exe')),
            '-v', 'error', '-show_format', '-show_streams', '-of', 'json', str(movie)]))
        duration = float(probe['format']['duration'])
        tile = Image.new('RGB', (960, 296), 'black')
        draw = ImageDraw.Draw(tile)
        draw.text((8, 4), f'{movie.name}  {duration:.3f}s', fill='white')
        times = [duration * f for f in (.18, .5, .82)]
        for i, t in enumerate(times):
            path = args.output / f'{movie.stem}-{i}.jpg'
            subprocess.run([str(args.ffmpeg), '-v', 'error', '-nostdin', '-ss', str(t), '-i', str(movie),
                '-frames:v', '1', '-vf', 'scale=320:180', '-y', str(path)], check=True)
            tile.paste(Image.open(path), (i * 320, 28))
            draw.text((i * 320 + 8, 214), f'{t:.2f}s', fill='white')
        tile.save(args.output / (movie.stem + '-contact.jpg'))
        records.append({'file': movie.name, 'duration': duration, 'probe': probe})
        print(movie.name, duration, flush=True)
    (args.output / 'probes.json').write_text(json.dumps(records, indent=2))
    for start in range(0, len(records), 7):
        sheet = Image.new('RGB', (960, 296 * min(7, len(records)-start)), 'black')
        for i, rec in enumerate(records[start:start+7]):
            sheet.paste(Image.open(args.output / (pathlib.Path(rec['file']).stem+'-contact.jpg')), (0, i*296))
        sheet.save(args.output / f'contact-{start//7+1}.jpg')


def analyze(args):
    import mmap
    import re
    import struct
    from collections import Counter
    with (args.output / 'code-trace.txt').open('w') as out:
        for f in (ROOT / 'kk/generated/default').glob('*.cpp'):
            s = f.read_text()
            for name in ['sub_8253DB80', 'sub_8253E398']:
                start = s.find('DEFINE_REX_FUNC(' + name + ')')
                if start >= 0:
                    end = s.find('DEFINE_REX_FUNC(', start + 1)
                    out.write(str(f) + '\n' + s[start:end] + '\n')
    keys = [int(f.stem[3:], 16) for f in args.movies.glob('*.mp4')]
    with (args.output / 'key-evidence.txt').open('w') as out:
        for f in args.game.rglob('*'):
            if not f.is_file() or f.stat().st_size == 0:
                continue
            with f.open('rb') as stream, mmap.mmap(stream.fileno(), 0, access=mmap.ACCESS_READ) as data:
                for pattern in [b'BK_', b'B\0K\0_\0']:
                    positions = [m.start() for m in re.finditer(re.escape(pattern), data)]
                    if positions:
                        out.write(f'{f.relative_to(args.game)} {pattern!r}: {positions[:30]} total={len(positions)}\n')
                if f.suffix.lower() not in ['.bf', '.xex']:
                    continue
                if f.suffix.lower() == '.bf':
                    table, entries = 0x2c, []
                    for _ in range(64):
                        count, _, start, nxt, _, _ = struct.unpack_from('<6I', data, table)
                        entries += [struct.unpack_from('<II', data, start+i*8) for i in range(count)]
                        if nxt == 0xffffffff:
                            break
                        table = nxt - 24
                    out.write(f'{f.name} matching table keys: {[(hex(k), pos) for pos,k in entries if k & 0xffff in keys]}\n')
                    continue
                for endian in ['<', '>']:
                    hits = []
                    for key in keys:
                        for prefix in [0, 0xff000000, 0xff800000, 0x3d000000]:
                            word = prefix | key
                            pos = data.find(struct.pack(endian + 'I', word))
                            if pos >= 0:
                                hits.append((f'{word:08X}', pos))
                    out.write(f'{f.name} {endian} exact keys: {hits}\n')
        # The english chapter-name text pack extracted using launcher_art.cpp's key.
        data = (args.output / 'KKMaps.bf-FD10C45A.bin').read_bytes()
        needle = struct.pack('<II', 0x4902dd68, 0xffffffff)
        at = data.find(needle)
        size, count = struct.unpack_from('<II', data, at-8)
        strings = at + count * 24
        names = [''] * 42
        for i in range(count):
            key, _, offset = struct.unpack_from('<III', data, at+i*24)
            if 0 <= key - 0x4902dd68 < 42:
                pos = strings + offset
                names[key-0x4902dd68] = data[pos:data.index(b'\0',pos)].decode('cp1252')
        (args.output / 'chapter-names.json').write_text(json.dumps(names, indent=2))
        # Summarize distinctive printable level identifiers in packs whose low keys match clips.
        for key in keys:
            pack = args.output / f'KKMaps.bf-FF00{key:04X}.bin'
            if not pack.exists():
                continue
            raw = pack.read_bytes()
            identifiers = [s.decode('ascii') for s in re.findall(rb'[A-Za-z_][A-Za-z0-9_ /.-]{6,110}', raw)
                           if any(t in s.lower() for t in [b'level', b'map', b'load', b'.wol', b'.wow'])]
            out.write(f'FF00{key:04X} identifiers: {identifiers[:25]}\n')


def catalog(args):
    """Evaluate the straight-line catalog registration, without executing the game."""
    import re
    import struct
    source = (args.output / 'code-trace.txt').read_text()
    source = source[source.index('DEFINE_REX_FUNC(sub_8253E398)'):]
    regs, mem = [0] * 32, {}
    regs[1] = 0x1000000
    mem[0x82cc9354] = 0x90000000
    def reg(s):
        return int(s.strip()[1:])
    for line in source.splitlines():
        if '// ' not in line:
            continue
        ins = line.split('// ', 1)[1].strip()
        op, _, operands = ins.partition(' ')
        a = operands.split(',')
        if op == 'bl':
            if mem.get(0x90000000 + 3052, 0) > 10:
                break
            continue
        if op in ['li', 'lis']:
            regs[reg(a[0])] = int(a[1]) * (65536 if op == 'lis' else 1) & 0xffffffff
        elif op in ['addi', 'addis', 'ori']:
            x, y, z = reg(a[0]), reg(a[1]), int(a[2])
            regs[x] = ((regs[y] | z) if op == 'ori' else regs[y] + z * (65536 if op == 'addis' else 1)) & 0xffffffff
        elif op == 'mr':
            regs[reg(a[0])] = regs[reg(a[1])]
        elif op == 'rlwinm':
            x,y,n,mb,me = reg(a[0]),reg(a[1]),*map(int,a[2:])
            mask = sum(1 << (31-i) for i in range(mb, me+1))
            regs[x] = ((regs[y] << n | regs[y] >> (32-n)) & mask) & 0xffffffff
        elif op in ['lwz', 'stw', 'stwu']:
            m = re.fullmatch(r'(-?\d+)\(r(\d+)\)', a[1])
            if not m:
                continue
            offset, b = map(int, m.groups())
            addr = (regs[b] + offset) & 0xffffffff
            if op == 'lwz': regs[reg(a[0])] = mem.get(addr, 0)
            else: mem[addr] = regs[reg(a[0])]
            if op == 'stwu': regs[b] = addr
        elif op == 'stwx':
            mem[(regs[reg(a[1])] + regs[reg(a[2])]) & 0xffffffff] = regs[reg(a[0])]
    count = mem.get(0x90000000+3052, 0)
    image = (ROOT / 'kk/image.bin').read_bytes()
    rows = []
    for i in range(count+1):
        ptr = mem.get(0x90000000+(462+i)*4, 0)
        offset = ptr-0x82000000
        name = image[offset:image.find(b'\0',offset)].decode('ascii',errors='replace') if 0 <= offset < len(image) else ''
        rows.append({'index':i, 'level':mem.get(0x90000000+(9+i)*4,0),
                     'world':f'{mem.get(0x90000000+(160+i)*4,0):08X}', 'name':name})
    (args.output / 'catalog.json').write_text(json.dumps(rows,indent=2))
    print(json.dumps(rows, indent=2))


def pictures(args):
    """Reuse the launcher's own DXT5 decoder; write only private raw pixels."""
    import struct
    from PIL import Image, ImageDraw
    source = (ROOT / 'kk/src/launcher_art.cpp').read_text()
    prefix = '\n'.join('#include <' + h + '>' for h in
                       ['algorithm','cstdint','cstring','fstream','filesystem','vector','string','cstdio','iostream'])
    prefix += '\nstruct Image { int width=0,height=0;std::vector<uint8_t> rgba; };\n'
    prefix += source[source.index('uint32_t LE32('):source.index('uint64_t FindBigEntry(')]
    prefix += source[source.index('uint32_t TiledOffset('):source.index('// The front end\'s logo:')]
    prefix += source[source.index('Image DecodeDxt5('):source.index('Image FindButtonSheet(')]
    prefix += 'constexpr int kChapterNames = 42;\n'
    prefix += source[source.index('constexpr int kChapterPicture['):source.index('// Writes the names (every language)')]
    prefix += r'''
int main(int argc,char** argv) {
 std::ifstream f(argv[1],std::ios::binary|std::ios::ate);size_t n=f.tellg();f.seekg(0);
 std::vector<uint8_t> data(n);f.read((char*)data.data(),n);
 auto pics=FindChapterPictures(data);std::cout<<pics.size()<<" pictures\n";
 for(int i=0;i<42;++i) {if(kChapterPicture[i]<0)continue;
  auto img=DecodeDxt5(pics[kChapterPicture[i]],256,128);
  std::ofstream out(std::filesystem::path(argv[2])/(std::to_string(i)+".rgba"),std::ios::binary);
  out.write((char*)img.rgba.data(),img.rgba.size());
 }
}
'''
    cpp = args.output / 'pictures.cpp'
    cpp.write_text(prefix)
    exe = args.output / 'pictures.exe'
    subprocess.run(['cl.exe','/nologo','/O2','/EHsc','/std:c++20',str(cpp),'/Fe:'+str(exe),'/Fo:'+str(args.output/'pictures.obj')],check=True)
    subprocess.run([str(exe),str(args.output/'KKTextures.bf-FF80C45A.bin'),str(args.output)],check=True)
    names=json.loads((args.output/'chapter-names.json').read_text())
    sheet=Image.new('RGB',(1000,210*10),'black')
    d=ImageDraw.Draw(sheet)
    for i in range(38):
        x,y=(i%4)*250,(i//4)*210
        im=Image.frombytes('RGBA',(256,128),(args.output/f'{i}.rgba').read_bytes()).convert('RGB').crop((0,18,256,110))
        im.thumbnail((245,160));sheet.paste(im,(x,y+30));d.text((x,y+5),f'{i}: {names[i]}',fill='white')
    sheet.save(args.output/'chapter-pictures.jpg')


def decode(args):
    """Headless check built from the actual production decoder functions."""
    if not args.clip:
        raise SystemExit('decode needs at least one --clip local filename')
    source = (ROOT / 'kk/src/ps2_movies.cpp').read_text()
    body = source[source.index('using Microsoft::WRL::ComPtr;'):source.index('class MovieDialog final')]
    includes = ['windows.h','mfapi.h','mfidl.h','mfreadwrite.h','propvarutil.h','wrl/client.h',
                'algorithm','atomic','chrono','condition_variable','cmath','cstdint','cstdio',
                'cstring','deque','filesystem','format','fstream','iostream','memory','mutex',
                'stdexcept','string','thread','vector','limits']
    prefix = '#define WIN32_LEAN_AND_MEAN\n#define NOMINMAX\n'
    prefix += '\n'.join('#include <'+h+'>' for h in includes)
    prefix += '\n#include "' + (ROOT/'kk/src/ps2_movies_logic.h').as_posix() + '"\n'
    prefix += '''
template<typename... A> void Log(std::format_string<A...> s,A&&... a) {
 std::cout<<std::format(s,std::forward<A>(a)...)<<std::endl;
}
#define REXLOG_INFO(...) Log(__VA_ARGS__)
#define REXLOG_WARN(...) Log(__VA_ARGS__)
void RequestUiPaint() {}
namespace kk::movies { namespace {
'''
    suffix = r'''
}}
int main(int argc,char**argv) {
 if(argc!=3)return 2;
 using namespace kk::movies;
 auto job=std::make_shared<Job>(); job->file=argv[1];
 int frames=0; int64_t previous=-1,first=-1;bool ordered=true,wrote=false;
 std::thread worker([job]{Decode(job);});
 auto until=Now()+90;
 while(!job->done && Now()<until) {
  bool available=false;
  {
   std::lock_guard guard(job->mutex);
   if(job->primed && !job->frames.empty()) {
    auto frame=std::move(job->frames.front());job->frames.pop_front();available=true;
    if(frame.pts<previous)ordered=false;previous=frame.pts;if(first<0)first=frame.pts;
    ++frames;
    if(!wrote && frame.pts>=10000000) {
     wrote=true;
     std::ofstream out(argv[2],std::ios::binary);out.write((char*)frame.rgba.data(),frame.rgba.size());
    }
   }
   if(job->eof && job->frames.empty())break;
  }
  job->cv.notify_all();
  if(!available)std::this_thread::sleep_for(std::chrono::milliseconds(1));
 }
 if(Now()>=until)End(job,"test timeout");
 worker.join();
 double square=0;
 for(size_t i=0;i+1<job->audio.size();i+=2) {int16_t v;std::memcpy(&v,job->audio.data()+i,2);square+=double(v)*v;}
 double rms=job->audio.empty()?0:std::sqrt(square/(job->audio.size()/2));
 std::cout<<"frames="<<frames<<" decoded="<<job->decoded<<" ordered="<<ordered
          <<" first_pts="<<first<<" last_pts="<<previous
          <<" dimensions="<<job->width<<"x"<<job->height<<" duration="<<job->duration
          <<" audio_seconds="<<double(job->audio.size())/(kRate*kFrameBytes)<<" rms="<<rms<<"\n";
 return (ordered && ((frames>0 && job->eof) || (frames==0 && job->decoded==3 && job->done)))?0:1;
}
'''
    cpp=args.output/'decoder-check.cpp';cpp.write_text(prefix+body+suffix)
    exe=args.output/'decoder-check.exe'
    subprocess.run(['cl.exe','/nologo','/O2','/EHsc','/std:c++20',str(cpp),'/Fe:'+str(exe),
                    '/Fo:'+str(args.output/'decoder-check.obj'),'/link','mfplat.lib','mfreadwrite.lib',
                    'mfuuid.lib','propsys.lib','ole32.lib'],check=True)
    for clip in args.clip:
        import time
        start=time.perf_counter()
        subprocess.run([str(exe),str(args.movies/clip),str(args.output/(clip+'.rgba'))],check=True)
        print(f'{clip}: decode elapsed {time.perf_counter()-start:.3f}s',flush=True)


if __name__ == '__main__':
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['scan', 'frames', 'analyze', 'catalog', 'pictures', 'decode'])
    p.add_argument('--output', type=pathlib.Path, required=True)
    p.add_argument('--game', type=pathlib.Path, default=ROOT / 'kk/assets')
    p.add_argument('--movies', type=pathlib.Path, default=pathlib.Path('C:/Users/TekRa/Desktop/kk_upscaled_movies'))
    p.add_argument('--clip', action='append', default=[], help='Local movie filename for a headless decoder check')
    p.add_argument('--ffmpeg', type=pathlib.Path, default=pathlib.Path('F:/KK-native-renderer/downloads/ffmpeg-btbn/extracted/ffmpeg-master-latest-win64-gpl/bin/ffmpeg.exe'))
    a = p.parse_args()
    a.output.mkdir(parents=True, exist_ok=True)
    {'scan': scan, 'frames': frames, 'analyze': analyze, 'catalog':catalog, 'pictures':pictures, 'decode':decode}[a.mode](a)
