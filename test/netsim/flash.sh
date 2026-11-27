#!/bin/sh
# Flash an IDF build directory at 1 Mbaud, with esptool from the IDF Docker
# image (no local IDF needed).
#
#   TTY=/dev/serial/by-id/... test/netsim/flash.sh BUILD_DIR
#
# Environment: TTY (default /dev/ttyUSB0), CHIP (default esp32),
# IDF_IMAGE (default espressif/idf:v5.5.1). Use the /dev/serial/by-id/ path:
# ttyUSBn numbers change when boards are replugged.
set -eu
B=$(readlink -f "$1"); T=$(readlink -f "${TTY:-/dev/ttyUSB0}")
docker run --rm --user "$(id -u):$(id -g)" --group-add "$(stat -c %g "$T")" \
  --device "$T" -e HOME=/tmp -v "$B:$B" -w "$B" "${IDF_IMAGE:-espressif/idf:v5.5.1}" \
  bash -c ". /opt/esp/idf/export.sh >/dev/null 2>&1; python -m esptool --chip ${CHIP:-esp32} \
    -p $T -b 1000000 --before default_reset --after hard_reset write_flash @flash_args" 2>&1 | tail -4
