#!/usr/bin/env python3
"""Send images, GIF animations or the screen to the Cast app.

Usage:
  cast.py HOST photo.jpg cat.gif some_folder/   images in order, GIFs with their own timing
  cast.py HOST --screen                         screen capture (X11; --region x,y,w,h for a part)

Each frame is a 4 byte big-endian length and a baseline JPEG; the cube answers one byte
('K' shown, 'E' could not decode) and the next frame goes out only after that, so it never lags.
"""
import argparse
import io
import os
import socket
import struct
import sys
import time

from PIL import Image, ImageOps, ImageSequence

PORT = 8080
EXTS = {".jpg", ".jpeg", ".png", ".bmp", ".gif", ".webp"}


def to_jpeg(img, size, quality, fit):
    img = img.convert("RGB")
    if fit == "cover":
        img = ImageOps.fit(img, size, Image.BILINEAR)
    else:
        img = ImageOps.pad(img, size, Image.BILINEAR, color=(0, 0, 0))
    buf = io.BytesIO()
    # Baseline 4:2:0, the only kind TJpgDec decodes quickly (MCU 16x16)
    img.save(buf, "JPEG", quality=quality, progressive=False, subsampling=2, optimize=False)
    return buf.getvalue()


def file_frames(paths, hold):
    """(PIL image, seconds to show) for every image / GIF frame, folders expanded"""
    files = []
    for p in paths:
        if os.path.isdir(p):
            files += [os.path.join(p, n) for n in sorted(os.listdir(p))
                      if os.path.splitext(n)[1].lower() in EXTS]
        else:
            files.append(p)
    for f in files:
        with Image.open(f) as img:
            if getattr(img, "is_animated", False):
                for frame in ImageSequence.Iterator(img):
                    yield frame.copy(), frame.info.get("duration", 100) / 1000
            else:
                yield ImageOps.exif_transpose(img), hold


def screen_frames(region):
    from PIL import ImageGrab

    bbox = None
    if region:
        x, y, w, h = (int(v) for v in region.split(","))
        bbox = (x, y, x + w, y + h)
    while True:
        yield ImageGrab.grab(bbox=bbox), 0


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("host", help="address shown by the Cast app (the :port part is optional)")
    p.add_argument("files", nargs="*", help="images, GIFs or folders")
    p.add_argument("--screen", action="store_true", help="cast the screen")
    p.add_argument("--region", help="screen part to cast: x,y,w,h")
    p.add_argument("--size", default="240x240", help="frame WxH, smaller is faster (e.g. 160x160)")
    p.add_argument("--quality", type=int, default=70)
    p.add_argument("--fit", choices=("cover", "contain"), default="contain")
    p.add_argument("--hold", type=float, default=3.0, help="seconds per still image")
    p.add_argument("--loop", action="store_true", help="start over at the end")
    args = p.parse_args()
    if not args.screen and not args.files:
        p.error("give images / GIFs / folders, or --screen")

    host, _, port = args.host.partition(":")
    size = tuple(int(v) for v in args.size.lower().split("x"))
    sock = socket.create_connection((host, int(port or PORT)), timeout=10)
    sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)

    sent, sent_bytes, start, bad = 0, 0, time.time(), 0
    try:
        while True:
            frames = screen_frames(args.region) if args.screen else file_frames(args.files, args.hold)
            for img, seconds in frames:
                due = time.time() + seconds
                data = to_jpeg(img, size, args.quality, args.fit)
                sock.sendall(struct.pack(">I", len(data)) + data)
                ack = sock.recv(1)
                if not ack:
                    sys.exit("The cube closed the connection")
                bad += ack != b"K"
                sent += 1
                sent_bytes += len(data)
                elapsed = time.time() - start
                print(f"\r{sent} frames, {sent / elapsed:.1f} fps, {sent_bytes / sent / 1024:.1f} KB/frame"
                      + (f", {bad} not decoded" if bad else ""), end="", flush=True)
                delay = due - time.time()
                if delay > 0:
                    time.sleep(delay)
            if not args.loop:
                break
    except KeyboardInterrupt:
        pass
    finally:
        print()
        sock.close()


if __name__ == "__main__":
    main()
