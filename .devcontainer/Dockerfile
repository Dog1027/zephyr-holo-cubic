# Pin image versions so the toolchain is as reproducible as the Zephyr revision
# in west.yml. The Zephyr SDK in the image must match zephyr/SDK_VERSION of that
# revision (currently 1.0.1); when bumping Zephyr, check it and bump this too.
FROM ghcr.io/zephyrproject-rtos/zephyr-build:v0.30.0

COPY --from=ghcr.io/casey/just:1.59.0 /just /usr/local/bin/
