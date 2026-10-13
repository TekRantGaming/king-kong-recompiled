"""Make a review panel from game captures only, never from the desktop."""
import sys
from pathlib import Path
from PIL import Image, ImageDraw

paths=[Path(v) for v in sys.argv[2:]]
panel=Image.new('RGB',(640*len(paths),390),'#181818')
draw=ImageDraw.Draw(panel)
for i,path in enumerate(paths):
    with Image.open(path) as image:
        frame=image.convert('RGB')
        frame.thumbnail((640,360))
        panel.paste(frame,(i*640,25))
    draw.text((i*640+5,5),path.parent.name,fill='white')
panel.save(sys.argv[1])
