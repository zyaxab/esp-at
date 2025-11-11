### AT-W5500
This component aims to add support for the W5500 Ethernet PHY, which is not currently supported by the mainline ESP-AT firmware, due to it using SPI.

### Implementation
Due to the limitations of ethernet support in ESP-AT (see the next chapter), adding ethernet support is non-trivial.

While the ESP-AT does support ethernet commands, they can only be used on ESP32 chips. Because of this, instead of using the AT-Ethernet command-set, we create our own component & our own commands.

### Limitations
This is tested on the ESP32-C6 board, though all ESP32 chips should work.
The SPI pinouts are hard coded (in `at_w5500.c`) but it should be trivial to make them configurable via menuconfig.

This ethernet implementation does not perfectly interface with all of the ESP-AT commands. Consider the effect of the following statements:
1) ESP-AT only supports specific PHY chips, which use `MII` or `RMII`. No SPI based PHY chips are supported.
2) ESP32-C6 (and others) do not have `MII` and `RMII` peripherals.
3) The basic AT command implementations are closed source and exist in a prebuilt binary.

Because of this, (ESP32-C6 but this applies to other chips), the prebuilt ESP32-C6 AT firmware does not have ethernet implementation. While this is fine for most things, as `Netif` handles allows us to add our own network interfaces, it's problematic for some non-critical commands. For example `AT+CIFSR` can't display the ethernet interface addresses, even though we have ethernet implemented.

### Building
1. Add the `at_w5500` component into the build system by doing:
```bash
export AT_CUSTOM_COMPONENTS="path/to/esp-at/examples/at_w5500"
```

2. Compile & flash as normal.

### Commands
Realistically, you don't need to use these commands. By default, the default MAC address is used & DHCP is enabled. Unless using some weird network, this component should just work. The commands
can be handy for testing though.

#### AT+ETHIP
This command allows you to query & set the IP address of the ESP32 chip. Doing this disables DHCP until reset.
```
AT+ETHIP? // Get current IP address
AT+ETHIP=<"ip">[,<"gateway">,<"netmask">] // Set the IP
```
where:
- <"ip">: Specifies the IP of the device
- <"gateway">: Specifies the gateway address, by default XXX.XXX.XXX.01, where the blank parts correspond to the IP address.
- <"netmask">: Specifies the netmask, by default /24.

The response for a successful `AT+ETHIP` command is
```
+ETHIP:ip:<"ip">
+ETHIP:gateway:<"gateway">
+ETHIP:netmask:<"netmask">
```
#### AT+ETHMAC
This command allows you to query the MAC address for the W5500 PHY chip. The MAC address is 02:11:22:XX:XX:XX, where `XX` bytes correspond to the NIC octets of the ESP32 MAC address, stored in the EFUSE.

**Note:** Currently, the MAC address OUI has the `locally administered bit` set.
```
AT+ETHMAC?          // Get the current MAC address
```

On success, you get
```
+ETHMAC:<"mac">
```
where:
- <"mac">: Specifies the MAC address of the device, in the format of hexadecimal values seperated by colons (XX:XX:XX:XX:XX:XX).