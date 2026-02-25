## ESP-AT fork
This fork includes some features we need from `ESP-AT` that are not supported at the time of writing.

### Installing

Create a python venv
```
python venv env
source env/bin/activate
```

Fetch submodules (no need for recursive)
```
git submodule update --init
```

Run the ESP install script
```
./build.py install
```

### Building

To set the binary version use `./build.py menuconfig` and navigate to `Application Manager > Project Version` and set the version
To use our custom components `export AT_CUSTOM_COMPONENTS="/path/to/esp-at-fork/examples/at_http_get_to_ram /path/to/esp-at-fork/examples/at_w5500"` is required to be ran prior to builds commands, to example commands assume this is being built from the repo's root

Then to actually build
```
export AT_CUSTOM_COMPONENTS="examples/at_http_get_to_ram examples/at_w5500"
./build.py build
```

## Intent
This repo should be relatively stale and binaries provided to any project using them