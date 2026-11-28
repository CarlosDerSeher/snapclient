#!/bin/bash
# Build every PlatformIO env from platformio.ini with idf.py in the IDF Docker
# image, several at a time, without touching the root sdkconfig or build/.
#
#   test/buildall/buildall.sh OUT_DIR [JOBS]      (from the repository root)
#
# Each env gets OUT_DIR/<env>/ (build dir with its own sdkconfig) and
# OUT_DIR/<env>.log; OUT_DIR/summary.txt has one line per env:
#   <env> exit <code> warnings <n>
# where n counts compiler warnings and errors in the project's own sources
# (main/ and components/, not managed_components/ or the submodules' code).
# JOBS defaults to 4; each job takes about 1 GB of RAM and runs ~26 compilers
# itself (ninja), so more jobs overload the PC. A full run builds all
# envs from scratch; a second run with the same OUT_DIR only rebuilds changes.
# The *-debug envs build with -Werror, so they catch warnings the others don't.
# Environment: IDF_IMAGE (default espressif/idf:v5.5.5).
set -u
OUT=$(mkdir -p "$1" && cd "$1" && pwd); JOBS=${2:-4}; P=$(pwd)
export OUT P IMG=${IDF_IMAGE:-espressif/idf:v5.5.5}
one() {
  e=$1 t=$2 d=$3 B=$OUT/$1
  mkdir -p "$B"
  docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$P:$P" -v "$OUT:$OUT" -w "$P" "$IMG" \
    idf.py -B "$B" -D SDKCONFIG="$B/sdkconfig" -D "SDKCONFIG_DEFAULTS=$d" -D IDF_TARGET="$t" build \
    > "$B.log" 2>&1
  r=$?
  w=$(grep -E "^$P/(main|components)/[^ ]+:[0-9]+:[0-9]+: (warning|error)" "$B.log" | sort -u | wc -l)
  echo "$e exit $r warnings $w" >> "$OUT/summary.txt"
}
export -f one
: > "$OUT/summary.txt"
python3 "$P/test/buildall/envs.py" "$P/platformio.ini" | xargs -P "$JOBS" -L 1 bash -c 'one "$0" "$1" "$2"'
sort -o "$OUT/summary.txt" "$OUT/summary.txt"
cat "$OUT/summary.txt"
echo "$(grep -c ' exit 0 ' "$OUT/summary.txt") of $(wc -l < "$OUT/summary.txt") envs built"
