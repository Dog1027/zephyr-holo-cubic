# Zephyr project — shared just recipes (board-agnostic)
# https://just.systems
#
# Override any variable or recipe in the top-level justfile
# (requires `set allow-duplicate-variables` / `set allow-duplicate-recipes` there).

set shell := ["bash", "-c"]

topdir     := justfile_directory()
app_dir    := topdir / "app"
build_dir  := topdir / "build"
west       := "west"

board      := "native_sim"
build_args := ""
flash_args := ""

# Show available recipes
default:
    @just --list

check-env:
    @{{west}} topdir > /dev/null
    @echo "Zephyr environment check passed."

# Build (extra args pass through, e.g. `just build -p`)
build *args: check-env
    {{west}} build -b {{board}} -d {{build_dir}} {{app_dir}} {{build_args}} {{args}}

# Pristine rebuild
rebuild: (build "-p")

# Open menuconfig
config: check-env
    {{west}} build -b {{board}} -d {{build_dir}} -t menuconfig {{app_dir}}

# Flash the connected device
flash: check-env
    {{west}} flash -d {{build_dir}} {{flash_args}}

# Open serial monitor
monitor:
    @echo "No monitor configured — override 'monitor' in justfile." && exit 1

# Remove build directory
clean:
    rm -rf {{build_dir}}

# Build and flash
upload: build flash

# Build, flash, and open monitor
run: build flash monitor
