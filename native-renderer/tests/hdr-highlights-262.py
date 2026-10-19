"""Task 262: HDR highlight expansion evidence (no game data is committed; outputs go under runs/).

python312 -I hdr-highlights-262.py input <image> <out.rgba>        full-resolution SDR input for nr_hdr_output_tests
python312 -I hdr-highlights-262.py stats <base.f32> <new.f32> <name> [paper] [peak]
    prints share of pixels above paper white, max nits, mid-tone change; writes <name>.exr (FLOAT RGB, 1 = 80 nits)
    and <name>-crop.png (a small display-referred preview: 0..peak nits mapped to 0..1 sRGB, so paper white is grey)
"""
from pathlib import Path
import json, struct, sys
import numpy as np
from PIL import Image

def exr(path, rgb):
    h, w, _ = rgb.shape
    def attr(name, kind, value):
        return name.encode()+b"\0"+kind.encode()+b"\0"+struct.pack("<I",len(value))+value
    channels = b"".join(c+b"\0"+struct.pack("<iB3xii",2,0,1,1) for c in (b"B",b"G",b"R"))+b"\0"
    box = struct.pack("<4i",0,0,w-1,h-1)
    header = struct.pack("<II",20000630,2)
    header += attr("channels","chlist",channels)+attr("compression","compression",b"\0")
    header += attr("dataWindow","box2i",box)+attr("displayWindow","box2i",box)
    header += attr("lineOrder","lineOrder",b"\0")+attr("pixelAspectRatio","float",struct.pack("<f",1))
    header += attr("screenWindowCenter","v2f",struct.pack("<2f",0,0))
    header += attr("screenWindowWidth","float",struct.pack("<f",1))+b"\0"
    stride = w*3*4+8
    offsets = b"".join(struct.pack("<Q",len(header)+h*8+y*stride) for y in range(h))
    rows = b"".join(struct.pack("<iI",y,w*12)+rgb[y,:,2].astype("<f4").tobytes()+rgb[y,:,1].astype("<f4").tobytes()
                    +rgb[y,:,0].astype("<f4").tobytes() for y in range(h))
    Path(path).write_bytes(header+offsets+rows)

def load(path):
    d = Path(path).read_bytes(); w, h = struct.unpack_from("<II", d)
    return np.frombuffer(d, "<f4", offset=8).reshape(h, w, 4)[..., :3]

if sys.argv[1] == "input":
    im = Image.open(sys.argv[2]).convert("RGBA")
    Path(sys.argv[3]).write_bytes(struct.pack("<II", *im.size) + im.tobytes())
elif sys.argv[1] == "stats":
    base, new, name = load(sys.argv[2]), load(sys.argv[3]), sys.argv[4]
    paper = float(sys.argv[5]) if len(sys.argv) > 5 else 200; peak = float(sys.argv[6]) if len(sys.argv) > 6 else 1000
    luma = np.array([.2126, .7152, .0722], np.float32)
    yb, yn = base @ luma * 80, new @ luma * 80
    mid = (yb >= 0.05 * paper) & (yb <= 0.5 * paper)
    r = {"name": name, "pixels": int(yb.size),
         "above_paper_pct_before": round(float((yb > paper * 1.001).mean() * 100), 3),
         "above_paper_pct_after": round(float((yn > paper * 1.001).mean() * 100), 3),
         "above_2x_paper_pct_after": round(float((yn > 2 * paper).mean() * 100), 3),
         "max_component_nits_before": round(float(base.max() * 80), 1), "max_component_nits_after": round(float(new.max() * 80), 1),
         "max_luminance_nits_after": round(float(yn.max()), 1),
         "p99_9_luminance_nits_before": round(float(np.percentile(yb, 99.9)), 1),
         "p99_9_luminance_nits_after": round(float(np.percentile(yn, 99.9)), 1),
         "mean_luminance_nits_before": round(float(yb.mean()), 2), "mean_luminance_nits_after": round(float(yn.mean()), 2),
         "midtone_pixels_pct": round(float(mid.mean() * 100), 2),
         "midtone_mean_abs_change_pct": round(float((np.abs(yn[mid] - yb[mid]) / yb[mid]).mean() * 100) if mid.any() else 0, 4),
         "pixels_changed_pct": round(float((np.abs(new - base).max(2) > 0).mean() * 100), 3),
         "identical": bool(np.array_equal(base, new))}
    print(json.dumps(r))
    exr(name + ".exr", new)
    h, w, _ = new.shape
    crop = new[h // 4: h // 4 + h // 2, w // 4: w // 4 + w // 2] if "--full" not in sys.argv else new
    disp = np.clip(crop * 80 / peak, 0, 1)
    disp = np.where(disp <= 0.0031308, disp * 12.92, 1.055 * disp ** (1 / 2.4) - 0.055)
    Image.fromarray((disp * 255 + 0.5).astype(np.uint8)).resize((min(480, crop.shape[1]), min(480, crop.shape[1]) * crop.shape[0] // crop.shape[1])).save(name + "-crop.png")
