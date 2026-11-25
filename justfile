# Zephyr ESP32 project — just recipes
# https://just.systems
# Shared Zephyr recipes (build / rebuild / config / flash / clean / upload / run) live in zephyr.just;
# this file sets the board and overrides what is ESP32 specific.
# Override variables: just board=esp32_devkitc/esp32/procpu port=/dev/ttyUSB0 run

set allow-duplicate-variables
set allow-duplicate-recipes

import 'zephyr.just'

board := "esp32_devkitc/esp32/procpu"
baud  := "921600"
port  := `p=(/host/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_*-if00-port0); echo "${p[0]}"`
flash_args := "--esp-device " + port + " --esp-baud-rate " + baud
# Host tools (tools/*.py) need Pillow and pyserial, already in the dev container's Python.
# -I keeps the cwd and env out of sys.path
python := "python3 -I"

# Open serial monitor
monitor:
    {{west}} espressif monitor -p {{port}}

# Convert a GIF into a baseline JPEG sequence, e.g. just gif2jpg anim.gif out/anim 120x120
gif2jpg gif out size="240x240" quality="80" bg="000000":
    {{python}} {{topdir}}/tools/gif2jpg.py "{{gif}}" "{{out}}" --size {{size}} --quality {{quality}} --bg {{bg}}

# Convert all images in a folder to baseline JPEGs for the Photos app, e.g. just img2jpg ~/Pictures out/photo
img2jpg in out size="240x240" quality="85" fit="cover":
    {{python}} {{topdir}}/tools/img2jpg.py "{{in}}" "{{out}}" --size {{size}} --quality {{quality}} --fit {{fit}}

# Send images / GIFs / folders or the screen to the Cast app, e.g. just cast 192.168.1.20 cat.gif, just cast 192.168.1.20 --screen
cast host *args:
    {{python}} {{topdir}}/tools/cast.py {{host}} {{args}}

# Test the Share app on the device over Wi-Fi: PIN, list, upload, download, mkdir, delete (close any serial monitor first)
test-share:
    {{python}} {{topdir}}/tools/share_test.py {{port}}

# Save the screen as a PNG: just shot OUT.png ["shell command" ...] (close any serial monitor first)
[positional-arguments]
shot out *commands:
    {{python}} {{topdir}}/tools/screenshot.py {{port}} "$@"

# Reset the board, open and close every app over the shell, report errors and stack usage (close any serial monitor first)
test:
    {{python}} {{topdir}}/tools/serial_test.py {{port}} {{build_dir}}/serial_test.log
