### AT Socket
This component replaces the ESP-AT TCP/IP socket commands (`AT+CIPSTART`, `AT+CIPSENDL`, `AT+CIPRECVDATA`, `AT+CIPCLOSE`, `AT+CIPSERVER`) & their `+IPD` URC with our own commands & events.

### Why
The stock commands write their responses in several pieces, & nothing serializes those writes against URCs. A `+IPD` can land between them, for example between `+CIPRECVDATA:<len>,` & the payload. The host then counts the URC as payload & the read gets corrupted without any error.

This component sends every response & event in a single write. The UART driver writes each call atomically, so nothing can end up inside a payload. Each read also reports how many bytes are left, so the host doesn't need to count bytes from events.

The stock commands still work, but don't mix them with these on the same link IDs. The links are separate from the stock ones.

### Limitations
- 5 links, numbered 0 - 4 (`AT_SOCK_MAX_LINKS`) & 2 listening ports (`AT_SOCK_MAX_LISTENERS`).
- Each open link buffers up to 4 KiB of received data (`AT_SOCK_RX_BUFFER_SIZE`). When the buffer is full, the ESP stops reading & TCP flow control slows the remote down.
- UDP links are only read when there's room for a whole datagram (1472 bytes), so datagrams don't get truncated.
- No TLS. UDP is connected only (one remote host & port per link).
- Listening is TCP & IPv4 only.

### Building
1. Add the `at_socket` component into the build system by doing:
```bash
export AT_CUSTOM_COMPONENTS="path/to/esp-at/examples/at_socket"
```

2. Compile & flash as normal.

### Detecting support
Every command supports the test form, which responds with the amount of links:
```
AT+SOCKOPEN=?
+SOCKOPEN:5
OK
```

Stock firmware responds with `ERROR`, so hosts can fall back to the stock commands. Check for the `+SOCKOPEN:` line rather than `OK`, since stock firmware may print `ERR CODE:` before `ERROR`.

### Commands
#### AT+SOCKOPEN
Connect to a remote host.
```
AT+SOCKOPEN=<link_id>,<"type">,<"host">,<port>
```
where:
- <link_id>: Specifies the link to use, 0 - 4. Must not already be in use
- <"type">: `"TCP"` or `"UDP"`
- <"host">: Specifies the host name or IP address
- <port>: Specifies the remote port

Responds with `OK` once connected, or `ERROR` if the link is in use, the host can't be resolved or connecting takes more than 8 seconds.

#### AT+SOCKSEND
Send data on a link.
```
AT+SOCKSEND=<link_id>,<length>
```
where:
- <link_id>: Specifies the link to send on
- <length>: Specifies how many bytes will be sent

The ESP responds with `OK` followed by `>`. After that, send exactly `<length>` bytes of raw data. When they've been sent, the response is `SEND OK`, or `SEND FAIL` on failure.

If no data arrives for 5 seconds, the ESP stops waiting & responds `SEND FAIL`. If sending fails partway through, the ESP still reads the rest of the data so it doesn't get treated as commands.

#### AT+SOCKRECV
Read received data from a link.
```
AT+SOCKRECV=<link_id>,<max_length>
```
where:
- <link_id>: Specifies the link to read from
- <max_length>: Specifies the maximum amount of bytes to read. Capped to 4096

On success, the response is
```
+SOCKRECV:<link_id>,<length>,<remaining>,<payload>
OK
```
where:
- <length>: Specifies how many bytes the payload contains, can be zero
- <remaining>: Specifies how many bytes are still buffered after this read
- <payload>: Contains the raw payload, followed by `\r\n`

When the remote has closed the link, the data that was left can still be read. The `+SOCK:<link_id>,CLOSED` event is sent after the read that empties the buffer.

#### AT+SOCKCLOSE
Close a link & release it. Data still buffered on the link is discarded.
```
AT+SOCKCLOSE=<link_id>
```

Closing a link that is already free responds with `OK`, since the remote may have closed it just before. Responds with `ERROR` only while the link is still connecting.

#### AT+SOCKLISTEN
Start or stop accepting TCP connections on a port.
```
AT+SOCKLISTEN=<port>[,<enable>]
```
where:
- <port>: Specifies the local port
- <enable>: `1` to start listening (default), `0` to stop. Stopping doesn't close connections that were already accepted

Each accepted connection takes the lowest free link. If no link is free, the connection is closed straight away.

### Events
Each event is sent as one line.

#### +SOCK:<link_id>,DATA,<available>
Data arrived on a link that had nothing buffered. It is not sent again until the host has read the link down to zero, so keep reading while `<remaining>` is non-zero.

The event can be sent while a `+SOCKRECV` is in progress. If it arrives after that read's response, the data may already have been read, so expect a read to return zero bytes now & then.

#### +SOCK:<link_id>,CLOSED
The remote closed the link, or it errored. This is only sent once all buffered data has been read, the same as the stock commands. The link is free again afterwards, there is no need to close it.

#### +SOCK:<link_id>,ACCEPT,<port>
A connection on `<port>` was accepted as `<link_id>`.

### Example usage
```
# Make sure to be connected via WiFi or Ethernet

AT+SOCKOPEN=0,"TCP","example.com",80
OK

AT+SOCKSEND=0,18
OK
>GET / HTTP/1.0\r\n\r\n
SEND OK

+SOCK:0,DATA,1460

AT+SOCKRECV=0,10
+SOCKRECV:0,10,1450,HTTP/1.0 2
OK

AT+SOCKRECV=0,2000
+SOCKRECV:0,1450,0,<payload>
+SOCK:0,CLOSED
OK
```
