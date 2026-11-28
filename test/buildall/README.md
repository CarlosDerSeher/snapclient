# buildall: build every PlatformIO env

Changes to `sdkconfig.defaults*`, the top-level `CMakeLists.txt`, IRAM code or anything shared
must build for all boards. ESP32 boards with PSRAM are tight on IRAM, and the `*-debug` envs
build with `-Werror`. `buildall.sh` builds each env from `platformio.ini` with `idf.py` in the
ESP-IDF Docker image, several in parallel, without touching the root `sdkconfig` or `build/`.

```sh
test/buildall/buildall.sh /tmp/buildall 4     # from the repository root; 4 parallel jobs
```

Needs Docker and the image `espressif/idf:v5.5.5` (`IDF_IMAGE` to change it). Each job needs
about 1 GB of RAM. The first run builds everything from scratch; a later run with the same
directory only rebuilds what changed.

Output in the given directory:
- `<env>/`: the build directory with the env's own `sdkconfig`; flash it with
  `test/netsim/flash.sh <dir>/<env>`.
- `<env>.log`: the build log.
- `summary.txt`: one line per env, `<env> exit <code> warnings <n>`, where `n` counts the
  compiler warnings and errors in `main/` and `components/`.

`envs.py` prints the list it builds: env name, IDF target (from the `env:base-<target>` the env
extends) and `SDKCONFIG_DEFAULTS`.
