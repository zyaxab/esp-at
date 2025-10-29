### AT HTTP Get to RAM
This component allows you to get an HTTP file and store it in the RAM of the ESP32 MCU. Later you can read the HTTP file via chunks.
This is verily heavily based on the `at_http_get_to_fatfs` example component, except that we don't use a non-volatile filesystem. This has the advantage of not consuming flash erase cycles & being slightly faster.

The main differences between this component & `at_http_get_to_fatfs` are:
1. Downloaded files are volatile, they get lost resets & power cycles.
2. You can only have one downloaded file at the time. Downloading a file when one is already downloaded overwrites the older file.
3. The maximum file size is 64 KiB. (Subject to change, currently hardcoded).

### Building
1. Add the `at_http_get_to_ram` component into the build system by doing
```bash
export AT_CUSTOM_COMPONENTS="path/to/esp-at/examples/at_http_get_to_ram"
```

2. Compile & flash.

### Commands
#### AT+HTTPGET_TO_RAM
This command lets you download an HTTP file to RAM. The command syntax is
```
AT+HTTPGET_TO_RAM=<"url">
```
where:
- <"url">: Specifies the URL of the HTTP file.

On success, the response is
```
+HTTPGET_TO_RAM:<content_length>
OK
```
where:
- <content_length>: Specifies the length of the downloaded file.

#### AT+HTTPGET_FROM_RAM
This command lets you read a chunk from a downloaded HTTP file that is stored in the ESP32 RAM. The command syntax is
```
AT+HTTPGET_FROM_RAM=<offset>,<len>
```
where:
- <offset>: Specifies where to from the file to start reading from
- <len>: How many bytes to read

On success, the response is
```
+HTTPGET_FROM_RAM:<chunk_len>,<payload>
```
where:
- <chunk_len>: Specifies the actual length of the chunk the ESP sent
- <payload>: Contains the raw payload

### Example usage
```
# Make sure to be connected via WiFi or Ethernet

AT+HTTPGET_TO_RAM="https://example.com"
+HTTPGET_TO_RAM:513
OK

AT+HTTPGET_FROM_RAM=0,10
+HTTPGET_FROM_RAM:10,<!doctype 
OK

AT+HTTPGET_FROM_RAM=10,10
+HTTPGET_FROM_RAM:10,html><html
OK
```