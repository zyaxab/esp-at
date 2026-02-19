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

Then to actually build
```
./build.py build
```

## Intent
This repo should be relatively stale and binaries provided to any project using them