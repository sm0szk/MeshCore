#pragma once

#include "helpers/bridges/BridgeBase.h"

#ifdef WITH_IP_BRIDGE

#include <WiFi.h>

#ifndef IP_BRIDGE_PORT
#define IP_BRIDGE_PORT 5001
#endif

#ifndef IP_BRIDGE_CONNECT_INTERVAL_MS
#define IP_BRIDGE_CONNECT_INTERVAL_MS 5000
#endif

#ifndef IP_BRIDGE_HOST
#error "Define IP_BRIDGE_HOST for IPBridge"
#endif

class IPBridge : public BridgeBase {
public:
  IPBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc);

  void begin() override;
  void end() override;
  void loop() override;
  void sendPacket(mesh::Packet *packet) override;
  void onPacketReceived(mesh::Packet *packet) override;
  void setHost(const char *host);
  const char *host() const;

private:
  static constexpr uint16_t FRAME_OVERHEAD = BRIDGE_MAGIC_SIZE + BRIDGE_LENGTH_SIZE + BRIDGE_CHECKSUM_SIZE;
  static constexpr uint16_t MAX_FRAME_SIZE = (MAX_TRANS_UNIT + 1) + FRAME_OVERHEAD;

  WiFiServer _server;
  WiFiClient _client;
  String _host;
  uint8_t _rx_buffer[MAX_FRAME_SIZE];
  uint16_t _rx_buffer_pos = 0;
  unsigned long _next_connect_attempt = 0;

  void acceptClient();
  void connectClient();
  void resetRx();
  void processByte(uint8_t byte);
  void discardClient();
  bool keepConnection(const WiFiClient &candidate) const;
};

#endif
