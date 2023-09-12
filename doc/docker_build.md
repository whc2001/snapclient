
# Building in Docker

### Prerequisities
- only Docker

No need to install ESP-IDF or anything else. All commands should be run in a project root.


## menuconfig
```
docker run --rm -it -v .:/project -w /project espressif/idf:v4.3.1 idf.py menuconfig
```

## Build
```
docker run --rm -it -v .:/project -w /project espressif/idf:v4.3.1 idf.py build
```

## Flash
Mapping of serial port to container is not simple in windows but you can merge all generated `bin` files into single firmware and flash the firmware manually using some windows tool.
 - [ESP Tool web flasher](https://espressif.github.io/esptool-js/)
 - [ESP32 Flash Download tool](https://www.espressif.com/en/support/download/other-tools)

#### Merge bins into single firmware bin file
```
docker run --rm -it -v .:/project -w /project/build espressif/idf:v4.3.1    //runs terminal in idf container

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
    espressif/idf:v4.3.1            // image name + version
    idf.py menuconfig               // run menuconfig
```