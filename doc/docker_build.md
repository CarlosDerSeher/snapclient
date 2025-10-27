
# Building in Docker

### Prerequisities
- Linux platform
- Docker
- Repository clone (refer to [README.md](/README.md))

No need to install ESP-IDF or anything else. All commands should be run in project root folder.

## Configure, Build and Flash
### Start an interactive IDF environnement
In this interactive shell you can run menuconfig, build, flash and monitor command.
```
docker run --rm -it -v .:/project -w /project -v /dev:/dev --privileged espressif/idf:v5.5.1
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
<span style="color: orange">If idf.py can't access to USB devices, try to restart your docker interactive shell in sudo.</span>

### Exit
Exit IDF monitor mode: `<Ctrl+]>`

Exit docker interactive shell: `exit`

## Specific actions
If you want to execute a specific command or to generate a reusable .bin file.
### menuconfig
```
docker run --rm -it -v .:/project -w /project -v /dev:/dev --privileged espressif/idf:v5.5.1 idf.py menuconfig
```

### Build
```
docker run --rm -it -v .:/project -w /project -v /dev:/dev --privileged espressif/idf:v5.5.1 idf.py build
```

### Flash
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
docker run --rm -it -v .:/project -w /project -v /dev:/dev --privileged espressif/idf:v5.5.1   //runs terminal in idf container

esptool.py --chip esp32 merge_bin --output firmware.bin @flash_args         // merges all bin files into firmware.bin
```

Write `build/firmware.bin` to ESP32 at address 0x0000




--------------------
### More details
```
docker run
    --rm                            // Removes container after exit
    -it                             // runs interactive terminal
    -v .:/project                   // maps current directory to /project in container
    -w /project                     // sets working directory inside a container to /project
    -v /dev:/dev                    // maps devices directory to acces USB and Serial devices inside docker
    --privileged                    // grants docker rights to acces host devices
    espressif/idf:v5.5.1            // image name + version
    idf.py menuconfig               // run menuconfig
```
