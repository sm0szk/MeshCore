#include "IPBridge.h"

#ifdef WITH_IP_BRIDGE

IPBridge::IPBridge(NodePrefs *prefs, mesh::PacketManager *mgr, mesh::RTCClock *rtc)
    : BridgeBase(prefs, mgr, rtc), _server(IP_BRIDGE_PORT) {}

void IPBridge::begin() {
  resetRx();
  _server.begin();
  _next_connect_attempt = 0;
  BRIDGE_DEBUG_PRINTLN("IP bridge listening and connecting to %s:%d", IP_BRIDGE_HOST, IP_BRIDGE_PORT);
  _initialized = true;
}

void IPBridge::end() {
  _client.stop();
  _server.end();
  resetRx();
  _initialized = false;
}

void IPBridge::discardClient() {
  _client.stop();
  resetRx();
}

void IPBridge::acceptClient() {
  WiFiClient incoming = _server.accept();
  if (incoming) {
    if (keepConnection(incoming)) {
      _client.stop();
      _client = incoming;
      resetRx();
      BRIDGE_DEBUG_PRINTLN("IP bridge accepted connection");
    } else {
      incoming.stop();
    }
  }
}

void IPBridge::connectClient() {
  if (_client.connected() || WiFi.status() != WL_CONNECTED || (long)(millis() - _next_connect_attempt) < 0) return;

  _next_connect_attempt = millis() + IP_BRIDGE_CONNECT_INTERVAL_MS;
  if (_client.connect(IP_BRIDGE_HOST, IP_BRIDGE_PORT)) {
    resetRx();
    BRIDGE_DEBUG_PRINTLN("IP bridge connected to %s:%d", IP_BRIDGE_HOST, IP_BRIDGE_PORT);
  } else {
    BRIDGE_DEBUG_PRINTLN("IP bridge connection failed");
  }
}

bool IPBridge::keepConnection(const WiFiClient &candidate) const {
  // Both peers may connect at the same time. Keep the socket owned by the
  // peer with the lower VPN address so both sides converge on one link.
  IPAddress local = WiFi.localIP();
  IPAddress remote = candidate.remoteIP();
  for (int index = 0; index < 4; index++) {
    if (local[index] != remote[index]) return local[index] < remote[index];
  }
  return true;
}

void IPBridge::loop() {
  if (!_initialized) return;

  acceptClient();
  connectClient();
  if (!_client || !_client.connected()) return;

  while (_client.available()) {
    processByte((uint8_t)_client.read());
    if (!_client.connected()) break;
  }
}

void IPBridge::processByte(uint8_t byte) {
  if (_rx_buffer_pos < 2) {
    uint8_t expected = (_rx_buffer_pos == 0) ? (BRIDGE_PACKET_MAGIC >> 8) : (BRIDGE_PACKET_MAGIC & 0xFF);
    if (byte == expected) {
      _rx_buffer[_rx_buffer_pos++] = byte;
    } else {
      resetRx();
      if (byte == (BRIDGE_PACKET_MAGIC >> 8)) _rx_buffer[_rx_buffer_pos++] = byte;
    }
    return;
  }

  if (_rx_buffer_pos >= sizeof(_rx_buffer)) {
    resetRx();
    return;
  }
  _rx_buffer[_rx_buffer_pos++] = byte;
  if (_rx_buffer_pos < 4) return;

  uint16_t len = ((uint16_t)_rx_buffer[2] << 8) | _rx_buffer[3];
  if (len > MAX_TRANS_UNIT + 1) {
    BRIDGE_DEBUG_PRINTLN("IP bridge invalid length %d", len);
    resetRx();
    return;
  }
  if (_rx_buffer_pos != len + FRAME_OVERHEAD) return;

  uint16_t received = ((uint16_t)_rx_buffer[4 + len] << 8) | _rx_buffer[5 + len];
  if (validateChecksum(_rx_buffer + 4, len, received)) {
    mesh::Packet *packet = _mgr->allocNew();
    if (packet && packet->readFrom(_rx_buffer + 4, len)) {
      onPacketReceived(packet);
    } else if (packet) {
      _mgr->free(packet);
    }
  } else {
    BRIDGE_DEBUG_PRINTLN("IP bridge checksum mismatch");
  }
  resetRx();
}

void IPBridge::sendPacket(mesh::Packet *packet) {
  if (!_initialized || !_client || !_client.connected() || !packet || _seen_packets.wasSeen(packet)) return;

  _seen_packets.markSeen(packet);
  uint8_t buffer[MAX_FRAME_SIZE];
  uint16_t len = packet->writeTo(buffer + 4);
  if (len > MAX_TRANS_UNIT + 1) return;

  buffer[0] = BRIDGE_PACKET_MAGIC >> 8;
  buffer[1] = BRIDGE_PACKET_MAGIC & 0xFF;
  buffer[2] = len >> 8;
  buffer[3] = len & 0xFF;
  uint16_t checksum = fletcher16(buffer + 4, len);
  buffer[4 + len] = checksum >> 8;
  buffer[5 + len] = checksum & 0xFF;
  _client.write(buffer, len + FRAME_OVERHEAD);
}

void IPBridge::onPacketReceived(mesh::Packet *packet) {
  handleReceivedPacket(packet);
}

#endif
