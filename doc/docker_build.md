
# Building in Docker

### Prerequisities
- Linux platform
- Docker
- Repository clone (refer to [README.md](/README.md))

No need to install ESP-IDF or anything else. All commands should be run in project root folder.

The commands below run the container as your own user (`--user`), so files created in the
project (`build/`, `sdkconfig`, ...) belong to you and not to root. `-e HOME=/tmp` gives the
tools in the container a writable home directory.
If you used the old root-based commands before, give the files back to your user once:
```
sudo chown -R $USER: build sdkconfig
```

### Serial port access
Only the serial port of the board is passed into the container (`--device`), no `--privileged`
mode is needed. Adjust `/dev/ttyUSB0` if your board shows up under a different name, e.g.
`/dev/ttyACM0` for boards with native USB (ESP32-S3). The device must be plugged in when the
container starts.

Serial ports belong to a group (`dialout` on most distributions, `uucp` on Arch Linux). The
container gets the port's group with `--group-add $(stat -c %g /dev/ttyUSB0)`, which works with any
group name.
To use the port outside of docker as well (e.g. `esptool` or the RFC2217 server below), add your
user to that group once and log out and in again:
```
sudo usermod -aG dialout $USER
```
On Arch Linux use `uucp` instead of `dialout`.

## Configure, Build and Flash
### Start an interactive IDF environnement
In this interactive shell you can run menuconfig, build, flash and monitor command.
```
docker run --rm -it --user $(id -u):$(id -g) --group-add $(stat -c %g /dev/ttyUSB0) --device /dev/ttyUSB0 -e HOME=/tmp -v .:/project -w /project espressif/idf:v5.5.1
```

### Configure
Then in your docker interactive shell, start by configuring for your platform. More info about the config in [README.md](/README.md#config).
```
idf.py menuconfig
```
Save with `<s>` and exit with `<q>`.

### Build, flash and monitor:
```
idf.py build flash monitor
```
Flashing is faster with a higher baud rate, e.g. `idf.py -b 1000000 flash monitor`.

### Exit
Exit IDF monitor mode: `<Ctrl+]>`

Exit docker interactive shell: `exit`

## Specific actions
If you want to execute a specific command or to generate a reusable .bin file.
### menuconfig
```
docker run --rm -it --user $(id -u):$(id -g) -e HOME=/tmp -v .:/project -w /project espressif/idf:v5.5.1 idf.py menuconfig
```

### Build
```
docker run --rm --user $(id -u):$(id -g) -e HOME=/tmp -v .:/project -w /project espressif/idf:v5.5.1 idf.py build
```

### Flash
```
docker run --rm -it --user $(id -u):$(id -g) --group-add $(stat -c %g /dev/ttyUSB0) --device /dev/ttyUSB0 -e HOME=/tmp -v .:/project -w /project espressif/idf:v5.5.1 idf.py -p /dev/ttyUSB0 -b 1000000 flash monitor
```

Mapping of serial port to container is not simple in windows but you can merge all generated `bin` files into single firmware and flash the firmware manually using some windows tool.
 - [ESP Tool web flasher](https://espressif.github.io/esptool-js/)
 - [ESP32 Flash Download tool](https://www.espressif.com/en/support/download/other-tools)

On MacOS / Linux  you need to install a small python package, make sure that pip is installed on your machine:

```
pip install esptool
```
After installation you can start the SerialServer with this command:
```
esp_rfc2217_server.py -v -p 4000 dev/serialDevice
```

Create now a new terminal run the docker container and flash it using this command:
```
idf.py --port 'rfc2217://host.docker.internal:4000?ign_set_control' flash monitor
```

#### Merge bins into single firmware bin file
```
docker run --rm -it --user $(id -u):$(id -g) -e HOME=/tmp -v .:/project -w /project espressif/idf:v5.5.1   //runs terminal in idf container

esptool.py --chip esp32 merge_bin --output firmware.bin @flash_args         // merges all bin files into firmware.bin
```

Write `build/firmware.bin` to ESP32 at address 0x0000




--------------------
### More details
```
docker run
    --rm                            // Removes container after exit
    -it                             // runs interactive terminal
    --user $(id -u):$(id -g)        // runs as your user, so created files belong to you
    --group-add <gid of the port>   // grants access to the serial port ($(stat -c %g /dev/ttyUSB0))
    --device /dev/ttyUSB0           // passes the board's serial port into the container
    -e HOME=/tmp                    // writable home directory for the tools
    -v .:/project                   // maps current directory to /project in container
    -w /project                     // sets working directory inside a container to /project
    espressif/idf:v5.5.1            // image name + version
    idf.py menuconfig               // run menuconfig
```
