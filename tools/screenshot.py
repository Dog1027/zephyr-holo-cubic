#!/usr/bin/env python3
"""Save the cube's screen as a PNG, over the serial console (the firmware's "shot" command).

Usage: screenshot.py PORT OUT.png [COMMAND ...] [-- OUT2.png [COMMAND ...]] ...
Each COMMAND is a shell line sent before that screenshot, with a short wait after it, e.g.
    screenshot.py PORT clock.png "app open Clock" -- dice.png "app open Dice" "ui tap"
Opening the port may reset the cube; the script then waits for it to boot. Close any serial
monitor first. Photos and animations drawn straight to the screen (Gallery, Cast) are not
in the screenshot.
"""
import base64, re, sys, time
import serial
from PIL import Image

CMD_WAIT_S = 1.5
BOOT_WAIT_S = 12
SHOT_TIMEOUT_S = 40
TRIES = 3
LV_COLOR_FORMAT_RGB565_SWAPPED = 0x1B   # LVGL 9: RGB565 is 0x12
LINE = re.compile(r"SHOT (BEGIN (\d+) (\d+) (\d+)|END|FAIL.*|(\d+) (\d+) (\d+) ([A-Za-z0-9+/=]+))$")


def send(ser, line):
    for ch in line + "\r":
        ser.write(ch.encode())
        time.sleep(0.01)


def read_for(ser, seconds):
    end, buf = time.time() + seconds, b""
    while time.time() < end:
        buf += ser.read(4096)
    return buf


def open_port(port):
    ser = serial.Serial()
    ser.port, ser.baudrate, ser.timeout = port, 115200, 0.1
    ser.rts = ser.dtr = False
    ser.open()
    if b"rst:" in read_for(ser, 1.5):
        read_for(ser, BOOT_WAIT_S)     # the cube reset when the port opened: let it boot
    return ser


def decode_row(img, y, x, data, swapped):
    rle = base64.b64decode(data, validate=True)
    if len(rle) % 3:
        raise ValueError("cut short")
    for i in range(0, len(rle), 3):
        p = rle[i + 1] | rle[i + 2] << 8
        if swapped:
            p = (p >> 8) | (p & 0xFF) << 8
        rgb = ((p >> 11 & 0x1F) * 255 // 31, (p >> 5 & 0x3F) * 255 // 63, (p & 0x1F) * 255 // 31)
        for _ in range(rle[i]):
            if x < img.width:
                img.putpixel((x, y), rgb)
            x += 1
    return x


def shot_once(ser):
    """One screenshot, or None when rows were lost (a log line mixed into one)."""
    ser.reset_input_buffer()
    send(ser, "shot")
    buf, img, swapped, rows = b"", None, False, set()
    end = time.time() + SHOT_TIMEOUT_S
    while time.time() < end:
        buf += ser.read(4096)
        *lines, buf = buf.split(b"\n")
        for raw in lines:
            m = LINE.search(raw.decode(errors="replace").strip())
            if not m:
                continue
            if m.group(2):
                img = Image.new("RGB", (int(m.group(2)), int(m.group(3))))
                swapped = int(m.group(4)) == LV_COLOR_FORMAT_RGB565_SWAPPED
            elif m.group(1).startswith("FAIL"):
                sys.exit(m.group(0))
            elif m.group(1) == "END":
                return img if img is not None and len(rows) == img.height else None
            elif img is not None:
                y, x, w = int(m.group(5)), int(m.group(6)), int(m.group(7))
                try:
                    if decode_row(img, y, x, m.group(8), swapped) == x + w:
                        rows.add(y)
                except ValueError:
                    pass
    sys.exit("No screenshot (is the firmware too old, or the port in use?)")


def shot(ser):
    for _ in range(TRIES):
        img = shot_once(ser)
        if img is not None:
            return img
    sys.exit(f"Rows kept getting lost, gave up after {TRIES} tries")


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    groups, cur = [], []
    for arg in sys.argv[2:]:
        if arg == "--":
            groups.append(cur)
            cur = []
        else:
            cur.append(arg)
    groups.append(cur)
    ser = open_port(sys.argv[1])
    for out, *commands in groups:
        for line in commands:
            send(ser, line)
            time.sleep(CMD_WAIT_S)
        shot(ser).save(out)
        print(out)


if __name__ == "__main__":
    main()
