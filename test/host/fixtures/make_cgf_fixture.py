#!/usr/bin/env python3
# Regenerates sample.cgf + sample.raw for test/host/cgf_test.cpp (a synthetic animation that exercises full frames,
# partial frames, an unchanged frame, long runs and noisy literals). Run from the repo root.
import random, subprocess, sys
from PIL import Image, ImageDraw
random.seed(7)
W, H, N = 41, 25, 9
frames = []
for k in range(N):
    im = Image.new("RGB", (W, H), (0, 0, 40))
    d = ImageDraw.Draw(im)
    d.rectangle([2 + k * 3, 4, 12 + k * 3, 14], fill=(255, 200, 0))
    if k in (3, 4):
        pass  # frame 4 is identical to frame 3 -> an "unchanged" frame
    else:
        for _ in range(40):
            im.putpixel((random.randrange(W), random.randrange(H)), tuple(random.randrange(256) for _ in range(3)))
    frames.append(im)
frames[4] = frames[3].copy()
d = "test/host/fixtures/"
frames[0].save(d + "sample.gif", save_all=True, append_images=frames[1:], duration=[40, 40, 50, 60, 60, 40, 40, 40, 90], loop=0, disposal=1)
sys.exit(subprocess.call([sys.executable, "tools/gif2cgf.py", d + "sample.gif", d + "sample.cgf", "--dump-canvas", d + "sample.raw", "--colors", "64"]))
