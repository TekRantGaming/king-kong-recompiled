"""Prepare SDR input, export actual GPU FP16 output as EXR, compare APIs.

python312 -I hdr-evidence-127.py input image.png output.rgba
python312 -I hdr-evidence-127.py output prefix
EXR is the simple scanline format described by OpenEXRFileLayout.html, FLOAT RGB,
uncompressed. Small 640px evidence avoids extra codecs and installation.
"""
from pathlib import Path
import json
import struct
import sys
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
    header += attr("screenWindowWidth","float",struct.pack("<f",1))
    header += attr("chromaticities","chromaticities",struct.pack("<8f",.64,.33,.30,.60,.15,.06,.3127,.3290))
    header += attr("comments","string",b"Linear scRGB BT.709; 1.0 = 80 cd/m2; GPU output, paper 200 nits, peak 1000 nits")+b"\0"
    stride = w*3*4+8
    offsets = b"".join(struct.pack("<Q",len(header)+h*8+y*stride) for y in range(h))
    with path.open("wb") as f:
        f.write(header+offsets)
        for y in range(h):
            pixels = rgb[y,:,::-1].astype("<f4").T.copy().tobytes()
            f.write(struct.pack("<ii",y,len(pixels))+pixels)

if sys.argv[1] == "input":
    im = Image.open(sys.argv[2]).convert("RGBA")
    im.thumbnail((640,360),Image.Resampling.LANCZOS)
    Path(sys.argv[3]).write_bytes(struct.pack("<II",*im.size)+im.tobytes())
    print("Prepared SDR input",im.size)
elif sys.argv[1] == "output":
    prefix=Path(sys.argv[2])
    outputs=[]
    for api in ("vulkan","d3d12"):
        path=Path(str(prefix)+"-"+api+".f32")
        data=path.read_bytes(); w,h=struct.unpack_from("<II",data)
        rgba=np.frombuffer(data,dtype="<f4",offset=8).reshape(h,w,4)
        assert np.isfinite(rgba).all() and rgba.min()>=0 and rgba.max()<=12.5
        outputs.append(rgba.copy())
        exr(path.with_suffix(".exr"),rgba[:,:,:3])
        # Preview is explicitly SDR, divided by paper white then encoded. The
        # EXR retains values above 1; no claim that this PNG displays HDR.
        linear=np.clip(rgba[:,:,:3]/2.5,0,1)
        srgb=np.where(linear<=.0031308,12.92*linear,1.055*linear**(1/2.4)-.055)
        Image.fromarray(np.round(srgb*255).astype("uint8")).save(Path(str(prefix)+"-"+api+"-preview.png"))
        print(api,"max nits",float(rgba[:,:,:3].max()*80),"EXR bytes",path.with_suffix(".exr").stat().st_size)
    delta=np.abs(outputs[0]-outputs[1])
    stats={"max_api_difference_scrgb":float(delta.max()),"mean_api_difference_scrgb":float(delta.mean()),
           "width":w,"height":h,"paper_nits":200,"peak_nits":1000}
    assert delta.max()<.02,stats
    Path(str(prefix)+"-summary.json").write_text(json.dumps(stats,indent=2)+"\n")
    print(json.dumps(stats))
elif sys.argv[1] == "capture":
    path=Path(sys.argv[2]);data=path.read_bytes();w,h=struct.unpack_from("<II",data)
    rgba=np.frombuffer(data,dtype="<f2",offset=8).astype("<f4").reshape(h,w,4)
    assert np.isfinite(rgba).all() and rgba.min()>=0 and rgba.max()<=12.51
    exr(path.with_suffix(".exr"),rgba[:,:,:3])
    linear=np.clip(rgba[:,:,:3]/2.5,0,1)
    srgb=np.where(linear<=.0031308,12.92*linear,1.055*linear**(1/2.4)-.055)
    Image.fromarray(np.round(srgb*255).astype("uint8")).save(path.with_suffix(".png"))
    stats={"width":w,"height":h,"color_space":"linear BT.709 scRGB, 1 = 80 nits", "paper_nits":200,"peak_nits":1000,
           "max_component_nits":float(rgba[:,:,:3].max()*80),
           "mean_luminance_nits":float((rgba[:,:,:3]@np.array([.2126,.7152,.0722])).mean()*80),
           "above_paper_white_percent":float((rgba[:,:,:3].max(axis=2)>2.5).mean()*100)}
    path.with_suffix(".json").write_text(json.dumps(stats,indent=2)+"\n")
    print(json.dumps(stats))
elif sys.argv[1] == "verify-capture":
    # FFmpeg's independent EXR decoder writes planar G,B,R float32 here.
    source=Path(sys.argv[2]);data=source.read_bytes();w,h=struct.unpack_from("<II",data)
    expected=np.frombuffer(data,dtype="<f2",offset=8).astype("<f4").reshape(h,w,4)[:,:,:3]
    planes=np.fromfile(sys.argv[3],dtype="<f4").reshape(3,h,w)
    decoded=np.stack((planes[2],planes[0],planes[1]),axis=-1)
    delta=float(np.abs(expected-decoded).max())
    assert delta==0,delta
    result={"independent_exr_decoder_max_difference":delta,"width":w,"height":h}
    Path(sys.argv[4]).write_text(json.dumps(result,indent=2)+"\n")
    print(json.dumps(result))
