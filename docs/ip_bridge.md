# IP bridge for Heltec V4

The Heltec V4 repeater can bridge MeshCore packets over a private VPN network. The bridge is full duplex and uses the same framed packet format as the RS232 bridge:

- TCP port `5001`: binary MeshCore bridge traffic
- TCP port `2323`: repeater CLI

Put both devices in the same WireGuard or Tailscale network. Flash the same `heltec_v4_repeater_ip` firmware on both devices. Each device listens for incoming connections and also initiates a connection to its configured peer.

## Local WiFi settings

Keep credentials out of the repository by adding them to `platformio.local.ini`:

```ini
[env:heltec_v4_repeater_ip]
build_flags =
  ${env:heltec_v4_repeater_ip.build_flags}
  -D WIFI_SSID=\"your-wifi\"
  -D WIFI_PWD=\"your-password\"
  -D IP_BRIDGE_HOST=\"100.64.0.1\"
```

Set `IP_BRIDGE_HOST` to the other repeater's VPN address. On repeater A it is repeater B's address; on repeater B it is repeater A's address. The same compiled firmware target can be used on both devices, with only this local build setting changed per device.

Build and upload:

```text
pio run -e heltec_v4_repeater_ip -t upload
```

Connect to the CLI through the VPN:

```text
telnet <vpn-address> 2323
```

The CLI currently has no separate network authentication. Do not expose port `2323` or `5001` to the public internet; restrict both ports to the VPN.
