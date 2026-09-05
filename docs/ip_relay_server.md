# Central IP relay server

An ESP32-S3-WROOM-1-N16R8 on an ESP32-S3-DevKitC-1 can run a central MeshCore IP relay without a LoRa radio. Heltec V4 repeaters connect to it over WiFi, WireGuard, or Tailscale.

The relay accepts up to eight TCP clients on port `5001`. Valid MeshCore bridge frames are forwarded to every other connected client. A Telnet CLI is available on port `2323`.

The server logs valid group text messages as `channel: message`. The public channel is decoded automatically. Up to four private channels can be enabled in the local `[relay_logging]` section with `channel_1_name` and `channel_1_key` (a 32-character hex key), through `_4`. Private keys are never printed.

The onboard RGB LED is off while WiFi is disconnected and green after DHCP has assigned an IP address. The default LED pin is GPIO48; override it with `-D RELAY_STATUS_LED_PIN=<pin>` if your ESP32-S3 board uses another RGB LED pin.

## Build and upload

WiFi settings are read from the local `[ip_wifi]` section in `platformio.local.ini`.

```text
pio run -e esp32_s3_ip_relay_server -t upload
```

After boot, read the relay address from the serial monitor at `115200 baud`:

```text
MeshCore IP relay setup
Relay IP: 100.64.0.10
Relay listening on TCP port 5001
```

WiFi can also be configured from the serial monitor. Settings are saved in the ESP32-S3 NVS and survive reboot:

```text
wifi status
wifi ssid <network-name>
wifi password <password>
wifi connect
wifi clear
```

The password is never printed by the CLI. `wifi clear` removes the saved values and restores the build-time defaults.

Connect to the gateway CLI over Telnet:

```text
telnet <gateway-ip> 2323
```

The Telnet CLI accepts one client at a time and has no authentication. Keep port `2323` inside the trusted LAN or VPN.

Set `IP_BRIDGE_HOST` in the Heltec local configuration to this relay address. Use the same Heltec firmware on every repeater.

The relay does not provide encryption or authentication. Keep port `5001` inside the VPN and do not expose it directly to the public internet.
