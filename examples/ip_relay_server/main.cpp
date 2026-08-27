#include <Arduino.h>
#include <WiFi.h>
#include <MeshCore.h>
#include <Adafruit_NeoPixel.h>

#ifndef WIFI_SSID
#error "Define WIFI_SSID in platformio.local.ini"
#endif
#ifndef WIFI_PWD
#error "Define WIFI_PWD in platformio.local.ini"
#endif
#ifndef IP_RELAY_PORT
#define IP_RELAY_PORT 5001
#endif
#ifndef IP_RELAY_MAX_CLIENTS
#define IP_RELAY_MAX_CLIENTS 8
#endif
#ifndef RELAY_STATUS_LED_PIN
#define RELAY_STATUS_LED_PIN 48
#endif

static constexpr uint16_t FRAME_MAGIC = 0xC03E;
static constexpr uint16_t FRAME_OVERHEAD = 6;
static constexpr uint16_t MAX_FRAME_SIZE = (MAX_TRANS_UNIT + 1) + FRAME_OVERHEAD;

struct RelayClient {
  WiFiClient socket;
  uint8_t frame[MAX_FRAME_SIZE];
  uint16_t framePosition = 0;
};

WiFiServer relayServer(IP_RELAY_PORT);
RelayClient relayClients[IP_RELAY_MAX_CLIENTS];
Adafruit_NeoPixel statusLed(1, RELAY_STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

void updateStatusLed() {
  bool hasDhcpAddress = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  statusLed.setPixelColor(0, hasDhcpAddress ? statusLed.Color(0, 32, 0) : 0);
  statusLed.show();
}

uint16_t fletcher16(const uint8_t *data, size_t length) {
  uint8_t sum1 = 0;
  uint8_t sum2 = 0;
  for (size_t index = 0; index < length; index++) {
    sum1 = (sum1 + data[index]) % 255;
    sum2 = (sum2 + sum1) % 255;
  }
  return (sum2 << 8) | sum1;
}

void resetFrame(RelayClient &relayClient) {
  relayClient.framePosition = 0;
}

void broadcastFrame(const uint8_t *frame, uint16_t frameLength, int sourceIndex) {
  for (int clientIndex = 0; clientIndex < IP_RELAY_MAX_CLIENTS; clientIndex++) {
    if (clientIndex == sourceIndex || !relayClients[clientIndex].socket.connected()) continue;
    relayClients[clientIndex].socket.write(frame, frameLength);
  }
}

void processByte(RelayClient &relayClient, int sourceIndex, uint8_t byte) {
  if (relayClient.framePosition < 2) {
    uint8_t expected = relayClient.framePosition == 0 ? FRAME_MAGIC >> 8 : FRAME_MAGIC & 0xFF;
    if (byte == expected) {
      relayClient.frame[relayClient.framePosition++] = byte;
    } else {
      resetFrame(relayClient);
      if (byte == (FRAME_MAGIC >> 8)) relayClient.frame[relayClient.framePosition++] = byte;
    }
    return;
  }

  relayClient.frame[relayClient.framePosition++] = byte;
  if (relayClient.framePosition < 4) return;

  uint16_t payloadLength = ((uint16_t)relayClient.frame[2] << 8) | relayClient.frame[3];
  if (payloadLength > MAX_TRANS_UNIT + 1 || payloadLength + FRAME_OVERHEAD > MAX_FRAME_SIZE) {
    resetFrame(relayClient);
    return;
  }
  if (relayClient.framePosition != payloadLength + FRAME_OVERHEAD) return;

  uint16_t receivedChecksum = ((uint16_t)relayClient.frame[4 + payloadLength] << 8) |
                              relayClient.frame[5 + payloadLength];
  if (fletcher16(relayClient.frame + 4, payloadLength) == receivedChecksum) {
    broadcastFrame(relayClient.frame, relayClient.framePosition, sourceIndex);
  }
  resetFrame(relayClient);
}

void acceptClients() {
  WiFiClient incoming = relayServer.accept();
  if (!incoming) return;

  for (int clientIndex = 0; clientIndex < IP_RELAY_MAX_CLIENTS; clientIndex++) {
    if (!relayClients[clientIndex].socket || !relayClients[clientIndex].socket.connected()) {
      relayClients[clientIndex].socket = incoming;
      resetFrame(relayClients[clientIndex]);
      relayClients[clientIndex].socket.print("MeshCore IP relay\r\n");
      Serial.printf("Relay client %d connected\r\n", clientIndex);
      return;
    }
  }
  incoming.stop();
  Serial.println("Relay full, rejected client");
}

void maintainClients() {
  for (int clientIndex = 0; clientIndex < IP_RELAY_MAX_CLIENTS; clientIndex++) {
    RelayClient &relayClient = relayClients[clientIndex];
    if (!relayClient.socket || !relayClient.socket.connected()) {
      if (relayClient.socket) relayClient.socket.stop();
      resetFrame(relayClient);
      continue;
    }
    while (relayClient.socket.available()) {
      processByte(relayClient, clientIndex, (uint8_t)relayClient.socket.read());
    }
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("MeshCore IP relay setup");
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PWD);

  unsigned long startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - startedAt < 20000) {
    delay(250);
    Serial.print('.');
  }
  Serial.println();
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("WiFi unavailable, retrying in loop");
  } else {
    Serial.print("Relay IP: ");
    Serial.println(WiFi.localIP());
  }
  updateStatusLed();

  relayServer.begin();
  Serial.printf("Relay listening on TCP port %d\r\n", IP_RELAY_PORT);
}

void loop() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.reconnect();
  }
  updateStatusLed();
  acceptClients();
  maintainClients();
  delay(1);
}
