## ESP-AT fork
This fork includes some features we need from `ESP-AT` that are not supported at the time of writing.

These features currently include:
1. Support for downloading HTTP files into RAM & reading the file chunk-wise. See `examples/at_http_get_to_ram`.
2. Support for the W5500 ethernet PHY chip.

### Bootstrapping
If you are using a Linux distro, that does not use the `apt` package manager (for example Fedora uses `dnf`), then define the following environment variable.
```bash
export HAS_IDF_PREREQUISITES="true"
```

ESP-IDF will attempt to download it's dependencies but it only supports `apt`. This skips that step but it means you need to download them yourself. The missing deps will be shown during later commands.

Secondly, we are compiling `ESP-AT` with custom components. These need to be enabled via another environment variable, as such:
```bash
export AT_CUSTOM_COMPONENTS="/path/to/esp-at-fork/examples/at_http_get_to_ram /path/to/esp-at-fork/examples/at_w5500"
```

It's recommended to add these to your `.bashrc` file (or equivalent).

Once the variables are set, run the following:
```bash
./build.py install
```

It will ask about the target MCU, choose them accordingly. Regarding `silent logs`, that feature can be left disabled. We are currently not limited by memory so the extra logs are nice. Feel free to enable it when using MCUs with smaller memories.

### Building
Before building, any settings that are needed can be change by doing
```bash
./build.py menuconfig
```

Building and flashing can be done with the following commands
```bash
./build.py build

./build.py flash -p /dev/ttyUSBN
```