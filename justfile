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
# Fixed path of the board's CP2102: other USB serial devices (FTDI) are plugged in too,
# and ttyUSB numbers change on replug. Override: just port=/dev/ttyUSB0 flash
port  := "/dev/serial/by-id/usb-Silicon_Labs_CP2102_USB_to_UART_Bridge_Controller_0001-if00-port0"
flash_args := "--esp-device " + port + " --esp-baud-rate " + baud
# Host tools (tools/*.py) use this project's own uv environment (pyproject.toml + uv.lock -> .venv),
# not the Zephyr workspace venv. -I keeps the cwd and env out of sys.path
python := "uv run --project " + topdir + " python -I"

# Open serial monitor
# `west espressif` reads runners.yaml from the build dir it finds on its own, before parsing
# any arguments (-e does not help), so it has to run from inside build_dir. build_dir is not in
# the west workspace, so ZEPHYR_BASE tells west where the workspace is.
monitor:
    cd {{build_dir}} && ZEPHYR_BASE={{workspace}}/zephyr uv run --no-project --python {{workspace}}/.venv west espressif monitor -p {{port}}

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

# Reset the board, open and close every app over the shell, report errors and stack usage (close any serial monitor first)
test:
    {{python}} {{topdir}}/tools/serial_test.py {{port}} {{build_dir}}/serial_test.log
