# Central IP relay server

An ESP32-S3-WROOM N8R8 can run a central MeshCore IP relay without a LoRa radio. Heltec V4 repeaters connect to it over WiFi, WireGuard, or Tailscale.

The relay accepts up to eight TCP clients on port `5001`. Valid MeshCore bridge frames are forwarded to every other connected client.

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

Set `IP_BRIDGE_HOST` in the Heltec local configuration to this relay address. Use the same Heltec firmware on every repeater.

The relay does not provide encryption or authentication. Keep port `5001` inside the VPN and do not expose it directly to the public internet.
