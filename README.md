# Snapcast client for ESP32

### Synchronous Multiroom audio streaming client for [Snapcast](https://github.com/badaix/snapcast) ported to ESP32

## Feature list
- FLAC, OPUS and PCM decoding currently supported
- Wifi setup from menuconfig
- WiFi provisioning via [ImprovWifi via Serial](https://www.improv-wifi.com/) (e.g. via [this link](https://web.esphome.io/))<br>
  Ensure your browser supports this, Chrome or Edge will handle serial communication just fine.
- Auto connect to snapcast server on network
- Buffers up to 758ms on Wroom modules (tested with 44100:16:2)
- Buffers more than enough on Wrover modules
- Multiroom sync delay controlled from Snapcast server (user has to ensure not to set this too high on the server)
- DSP / EQ functionality configurable through menuconfig and partly controllable through HTTP server running on ESP client (work in progress)

## Description
I have picked up the work from [bridadan](https://github.com/bridadan/libsnapcast) and [jorgenkraghjakobsen](https://github.com/jorgenkraghjakobsen/snapclient)
towards a ESP32 Snapcast client. It is a full featured snapcast client which
supports the codecs FLAC, OPUS and PCM 16bit audio streams with sample rates
up to 48Khz maybe more, I didn't test.

Please check out the task list and feel free to fill in.

I dropped the usage of ADF completely but copied stripped down, needed components to this project (using <b>ESP-ADF v2.6</b>).
This was necessary because ADF was using flac in closed source precompiled library
which made it impossible to get good results for multiroom syncing.

### Codebase

The codebase is split into components and build on <b>ESP-IDF v5.1.5</b>. I still
have some refactoring on the todo list as the concept has started to settle and
allow for new features can be added in a structured manner. In the code you
will find parts that are only partly related features and still not on the task
list. Also there is a lot of code clean up needed, as there is quite some dead code too.

Components
 - audio-board : taken from ADF, stripped down to strictly necessary parts for playback
 - audio-hal : taken from ADF, stripped down to strictly necessary parts for playback
 - audio-sal : taken from ADF, stripped down to strictly necessary parts for playback
 - custom_board : generic board component to support easy integration of DACs
 - dsp_processor : Audio Processor, low pass filters, effects, etc.
 - esp-dsp : Submodule to the ESP-ADF done by David Douard
 - esp-peripherals : taken from ADF, stripped down to strictly necessary parts for usage with Lyrat v4.3
 - flac : flac audio encoder/decoder full submodule
 - opus : Opus audio coder/decoder full submodule
 - libmedian: Median Filter implementation. Many thanks to [accabog](https://github.com/accabog/MedianFilter)
 - libbuffer : Generic buffer abstraction
 - lightsnapcast :
   * snapcast module, port of @bridadan scapcast packages decode library
   * player module, which is responsible for sync and low level I2S control
 - net_functions :
 - ota_server :
 - protocol :
 - rtprx : Alternative RTP audio client UDP low latency also opus based
 - websocket :
 - websocket_if :
 - improv_wifi : WiFi provisioning via [ImprovWifi via Serial](https://www.improv-wifi.com/)
 - network_interface : init code for wifi module and AP connection and ethernet init code
 - ui_http_server : work in progress control interface for DSP functions

The snapclient functionality are implemented in a task included in main - but
should be refactored to a component at some point.

I did my own syncing implementation which is different than jorgenkraghjakobsen's
approach in the original repository, at least regarding syncing itself. I tried to
replicate the behavior of how badaix did it for his original snapclients.

The snapclient frontend handles communication with the server and after
successfull hello handshake it dispatches packages from the server.
Normally these packages contain messages in the following order:

 - SERVER_SETTING : volume, mute state, playback delay etc
 - CODEC_HEADER : Setup client audio codec (FLAC, OPUS, OGG or PCM) bitrate, n
   channels and bits per sample
 - WIRE_CHUNK : Coded audio data, also I calculate chunk duration here after
   decoding is done using received CODEC_HEADER parameters
 - TIME : Ping pong time keeping packages to keep track of time diff from server
   to client

Each WIRE_CHUNK of audio data comes with a timestamp in server time and clients
can use information from TIME and SERVER_SETTING messages to determine when playback
has to be started. We handle this using a buffer with a length that compensate for
playback-delay, network jitter and DAC to speaker (determined through SERVER_SETTING).

In this implementation I have separated the sync task to a backend on the other
end of a freeRTOS queue. Now the front end just needs to pass on the decoded audio
data to the queue with the server timestamp and chunk size. The backend reads
timestamps and waits until the audio chunk has the correct playback-delay
to be written to the DAC amplifer speaker through i2s DMA. When the backend pipeline
is in sync, any offset gets corrected by inserting a single sample every chunk_ms,
which is determined by the server.


### Hardware
You will need an ESP32 or ESP32-S2 and an I2S DAC. For pinout see the default config options in menuconfig (Audio Board).

## Installation

Clone this repo:
```
git clone https://github.com/CarlosDerSeher/snapclient
cd snapclient
```

Update third party code (opus, flac, esp-dsp, improv_wifi):
```
git submodule update --init
```

### ESP-IDF environnement configuration
- <b>If you're on Windows :</b> Install [ESP-IDF v5.1.5](https://github.com/espressif/esp-idf/releases/tag/v5.1.5) locally ([More info](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/get-started/windows-setup-update.html)).
- <b>If you're on Linux (docker) :</b> Use the image for ESP-IDF by following [docker build](doc/docker_build.md) doc
- <b>If you're on Linux :</b> follow [official Espressif](https://docs.espressif.com/projects/esp-idf/en/stable/esp32/get-started/linux-macos-setup.html) instructions
  For debian based systems you'll need to do the following:
  ```
  sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
  mkdir -p ~/esp
  cd ~/esp
  git clone -b v5.1.5 --recursive https://github.com/espressif/esp-idf.git
  cd ~/esp/esp-idf
  ./install.sh esp32
  . ./export.sh
  ```

<a name="config"></a>
### Snapcast ESP Configuration
First copy one of the template sdkconfig files and rename it to sdkconfig

```
cp sdkconfig_lyrat_v4.3 sdkconfig
```

then configure your platform:

```
idf.py menuconfig
```
Configure to match your setup
  - <b>Audio HAL :</b> Choose your audio board
    - Lyrat (4.3, 4.2)
    - Lyrat TD (2.2, 2.1)
    - Lyrat Mini (1.1)
    - KORVO DU1906
    - ESP32-S2 Kaluga (1.2)
    - ESP-AI-Thinker-ES8388 (2.2)
    - Or a custom board
  - <b>Custom Audio Board :</b> Configure your DAC and GPIO
    - DAC Chip :
      - TI PCM51XX/TAS57XX DAC (PCM51XX are stereo DAC in TSSOP package and TAS57XX are class-D amp in HTSSOP package. Both have i2s input and i2c control)
      - TI PCM5102A DAC (Very basic stereo DAC WITHOUT i2c control)
      - TI TAS5805M DAC
      - Infineon MA120X0 (High power class-D amp in QFN package)
      - Analog Devices ADAU1961 (Stereo DAC with multiple analog inputs in LFCSP package)
      - Analog Devices MAX98357 (Very popular basic mono AMP without i2c control)
      - Princton Technology PT8211
    - DAC I2C control interface : Choose GPIO pin of your I2C line and address of the DAC. If your DAC doesn't support I2C (PCM5102A or equivalent), set them to -1.
    - I2C master interface : GPIO pin of your DAC I2S bus.
    - DAC interface configuration : Configure specific GPIO for your DAC functionnalities. Use `?` to have more info.
  - <b>ESP32 DSP processor config :</b>
    - DSP flow : Choose between Stereo, Bassboost, Bi-amp or Bass/Treble EQ. You can further configure it on the ESP web interface/
    - Use asm version of Biquad_f32 : Optimized version of the DSP algorithm only for ESP32. Don't work on ESP32-S2
    - Use software volume : Handle snapcast volume in the ESP. Activate this if your DAC do not provide a volume control (no I2C like PCM5102A or MAX98357)
  - <b>WiFi Configuration :</b>
    - WiFi Provisioning : Use the Espressif "ESP SoftAP Prov" APP to configure your wifi network.
    - SSID : The SSID to connect to or the provisioning SSID.
    - Password : The password of your WiFi network or the provisioning network.
    - Maximum retry: Use 0 for no limit.
  - <b>Snapclient configuration :</b>
    - Use mDNS : The client will search on the network for the snapserver automatically. Your network must support mDNS.
    - Snapserver host : IP or URL of the server if mDNS is disabled or the mDNS resolution fail.
    - Snapserver port : Port of your snapserver, default is 1704.
    - Snapclient name : The name under which your ESP will appear on the Snapserver.
    - HTTP Server Setting : The ESP creates a basic webpage. You can configure the port to view this page and configure the DSP.


### Compile and flash
```
idf.py build flash monitor
```

### Merge bin to flash at 0x0 with web.esphome.io

```
esptool.py --chip esp32  merge_bin -o merged.bin --flash_size 4MB --flash_freq 80m 0x1000 build/bootloader/bootloader.bin 0x8000 build/partition_table/partition-table.bin 0xd000 build/ota_data_initial.bin 0x10000 build/snapclient.bin 0x370000 build/storage.bin
```

## Test
Setup a snapcast server on your network

On a linux box:

Install [Snapcast](https://github.com/badaix/snapcast) and start the server. You won't need the following command if you installed it as a service.

    ./snapserver

Pipe some audio to the snapcast server fifo

    mplayer http://ice1.somafm.com/secretagent-128-aac -ao pcm:file=/tmp/snapfifo -af format=s16LE -srate 48000

Test the server config on other known platform

    ./snapclient  from the snapcast repo

Android : snapclient from the app play store

## OTA update
Update your client(s) over the air.

On a linux box:

```
cd snapclient
idf.py build
curl snapclient.local:8032 --data-binary @- < build/snapclient.bin
```
Replace `snapclient.local` with your clients IP address. If you have multiple clients you could use the Android or Web App to find out your clients IPs.

## Contribute

You are very welcome to help and provide [Pull
Requests](https://docs.github.com/en/github/collaborating-with-issues-and-pull-requests/about-pull-requests)
to the project.

We strongly suggest you activate [pre-commit](https://pre-commit.com) hooks in
this git repository before starting to hack and make commits.

Assuming you have `pre-commit` installed on your machine (using `pip install
pre-commit` or, on a debian-like system, `sudo apt install pre-commit`), type:

```
:~/snapclient$ pre-commit install
pre-commit installed at .git/hooks/pre-commit
```

Then on every `git commit`, a few sanity/formatting checks will be performed.


## Task list
- [ ] put kconfig to better locations in tree
- [ ] add missing codecs (ogg)
- [ ] dsp_processor: add equalizer
- [ ] Control interface for equalizer (component: ui_http_server)
- [ ] clean and polish code (remove all unused variables etc.)
- [ ] Improve Documentation
- [ ] Throw out ADF copied components from project tree and use CmakeLists.txt to pull in necessary files from ADF

## Minor task
- [ ] fill in missing component descriptions in Readme.md
