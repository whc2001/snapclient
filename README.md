# Snapcast client for ESP32

### Synchronous Multiroom audio streaming client for [Snapcast](https://github.com/badaix/snapcast) ported to ESP32

## Feature list
- FLAC decoding currently supported
- Wifi setup from menuconfig or through espressif Android App "SoftAP Prov"
- Auto connect to snapcast server on network
- Buffers up to 1000ms on Wroom modules (tested with 44100:16:2)
- Buffers more then enough on Wrover modules
- Multiroom sync delay controlled from Snapcast server (user has to ensure not to set this too high on the server)

## Description
I have continued the work from @badaix, @bridadan and @jorgenkraghjakobsen towards a ESP32 Snapcast
client. Currently it support basic features like multiroom sync, network
controlled volume and mute. For now it only support FLAC 16bit
audio streams with sample rates up to 48Khz maybe more, I didn't test.

Please check out the task list and feel free to fill in.

I dropped the usage of ADF completely but copied stripped down, needed components to this project.
This was necessary because ADF was using flac in closed source precompiled library
which made it impossible to get good results for multiroom syncing. IDF's I2S driver was also copied
to project's components and adapted. Originally it wasn't possible to pre load DMA buffers with audio
samples and therefore no precise sync could be achieved.

### Codebase

The codebase is split into components and build on ESP-IDF v4.3.1. I still
have some refactoring on the todo list as the concept has started to settle and
allow for new features can be added in a structured manner. In the code you
will find parts that are only partly related features and still not on the task
list. Also there is a lot of code clean up needed.

Components
 - audio-board : taken from ADF, stripped down to strictly necessary parts for usage with Lyrat v4.3
 - audio-hal : taken from ADF, stripped down to strictly necessary parts for usage with Lyrat v4.3
 - audio-sal : taken from ADF, stripped down to strictly necessary parts for usage with Lyrat v4.3
 - custom_board :
 - custom-driver : modified I2S driver from IDF v4.3.1 which supports preloading DMA buffers with valid data
 - dsp_processor : Audio Processor, low pass filters, effects, etc.
 - esp-dsp : Submodule to the ESP-ADF done by David Douard
 - esp-peripherals : taken from ADF, stripped down to strictly necessary parts for usage with Lyrat v4.3
 - flac : flac audio encoder/decoder full submodule
 - libmedian: Median Filter implementation. Many thanks to @accabog https://github.com/accabog/MedianFilter
 - libbuffer : Generic buffer abstraction
 - lightsnapcast :
   * snapcast module, port of @bridadan scapcast packages decode library
   * player module, which is responsible for sync and low level I2S control
 - net_functions :
 - opus : Opus audio coder/decoder full submodule
 - ota_server :
 - protocol :
 - rtprx : Alternative RTP audio client UDP low latency also opus based
 - websocket :
 - websocket_if :
 - wifi_interface : wifi provisoning and init code for wifi module and AP connection

The snapclient functionanlity are implemented in a task included in main - but
should be refactored to a component at some point.

I did my own syncing implementation which is different than @jorgenkraghjakobsen's
approach in the original repository, at least regarding syncing itself. I tried to
replicate the behaivior of how badaix did it for his original snapclients.

The snapclient frontend handles communication with the server and after
successfull hello hand shake it dispatches packages from the server.
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
has to be started. We handle this using a buffer with a length that compensate for for
playback-delay, network jitter and DAC to speaker (determined through SERVER_SETTING).

In this implementation I have separated the sync task to a backend on the other
end of a freeRTOS queue. Now the front end just needs to pass on the decoded audio
data to the queue with the server timestamp and chunk size. The backend reads
timestamps and waits until the audio chunk has the correct playback-delay
to be written to the DAC amplifer speaker through i2s DMA. When the backend pipeline
is in sync, any offset get rolled in by micro tuning the APLL on the ESP. No
sample manipulation needed.


### Hardware
    -   ESP pinout                         MA12070P
    ------------------------------------------------------
                              ->            I2S_BCK        Audio Clock 3.072 MHz
                              ->            I2S_WS         Frame Word Select or L/R
                              ->            GND            Ground
                              ->            I2S_DI         Audio data 24bits LSB first
                              ->            MCLK           Master clk connect to I2S_BCK
                              ->            I2C_SCL        I2C clock
                              ->            I2C_SDA        I2C Data
                              ->            GND            Ground
                              ->            NENABLE        Amplifier Enable active low
                              ->            NMUTE          Amplifier Mute active low


## Build

Clone this repo:

    git clone https://github.com/CarlosDerSeher/snapclient

Update third party code (opus, flac and esp-dsp):

    git submodule update --init

Configure to match your setup
  - Wifi network name and password
  - Audio codec/board setup

Use [docker](doc/docker_build.md) or local IDF installation

    idf.py menuconfig

Build, compile and flash:

    idf.py build flash monitor

## Test
Setup a snapcast server on your network

On a linux box:

Clone snapcast build and start the server

    ./snapserver

Pipe some audio to the snapcast server fifo

    mplayer http://ice1.somafm.com/secretagent-128-aac -ao pcm:file=/tmp/snapfifo -af format=s16LE -srate 48000

Test the server config on other knowen platform

    ./snapclient  from the snapcast repo

Android : snapclient from the app play store

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
- [ok] Fix to alinge with above
- [ok] put kconfig to better locations in tree
 * add codec description
- [ok] Integrate ESP wifi provision
- [ok] Find and connect to Avahi broadcasted Snapcast server name
- [ ] Add a client command interface layer like volume/mute control
- [ ] add missing codec's (ogg, pcm, opus)
- [ok] test esp-dsp functionality after ADF drop
- [ok] Check compatibility with different HW than Lyrat v4.3
- [ok] rework dsp_processor and test. At the moment only dspfStereo and dspfBassBoost will work. Also ensure/test we got enough RAM on WROVER modules
- [ ] reduce dsp_processor memory footprint
- [ ] dsp_processor: add equalizer
 * Control interface for equalizer
- [ ] clean and polish code (remove all unused variables etc.)

## Minor task
  - [ok] soft mute - play sample in buffer with decreasing volume
  - [ok] hard mute - using ADF's HAL
  - [ok] Startup: do not start parsing on samples to codec before sample ring buffer hits requested buffer size.
  - [ok] Start from empty buffer
  - [ ] fill in missing component descriptions in Readme.md
  - [ok] DAC latency setting from android app
