#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <MeshCore.h>
#include <Packet.h>
#include <Utils.h>
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
#ifndef IP_RELAY_SEEN_CACHE_SIZE
#define IP_RELAY_SEEN_CACHE_SIZE 256
#endif
#ifndef BRIDGE_DEBUG
#define BRIDGE_DEBUG 0
#endif

#ifndef IP_RELAY_PRIVATE_CHANNEL_1_NAME
#define IP_RELAY_PRIVATE_CHANNEL_1_NAME ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_1_KEY
#define IP_RELAY_PRIVATE_CHANNEL_1_KEY ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_2_NAME
#define IP_RELAY_PRIVATE_CHANNEL_2_NAME ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_2_KEY
#define IP_RELAY_PRIVATE_CHANNEL_2_KEY ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_3_NAME
#define IP_RELAY_PRIVATE_CHANNEL_3_NAME ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_3_KEY
#define IP_RELAY_PRIVATE_CHANNEL_3_KEY ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_4_NAME
#define IP_RELAY_PRIVATE_CHANNEL_4_NAME ""
#endif
#ifndef IP_RELAY_PRIVATE_CHANNEL_4_KEY
#define IP_RELAY_PRIVATE_CHANNEL_4_KEY ""
#endif

static constexpr uint16_t FRAME_MAGIC = 0xC03E;
static constexpr uint16_t FRAME_OVERHEAD = 6;
static constexpr uint16_t MAX_FRAME_SIZE = (MAX_TRANS_UNIT + 1) + FRAME_OVERHEAD;
static constexpr size_t SERIAL_COMMAND_SIZE = 128;
static constexpr unsigned long WIFI_RETRY_INTERVAL_MS = 10000;

struct RelayClient {
  WiFiClient socket;
  uint8_t frame[MAX_FRAME_SIZE];
  uint16_t framePosition = 0;
};

WiFiServer relayServer(IP_RELAY_PORT);
RelayClient relayClients[IP_RELAY_MAX_CLIENTS];
Preferences wifiPreferences;
String wifiSsid;
String wifiPassword;
char serialCommand[SERIAL_COMMAND_SIZE] = {};
size_t serialCommandLength = 0;
unsigned long nextWifiRetryAt = 0;
uint8_t seenPacketHashes[IP_RELAY_SEEN_CACHE_SIZE][MAX_HASH_SIZE] = {};
size_t nextSeenPacketHash = 0;
Adafruit_NeoPixel statusLed(1, RELAY_STATUS_LED_PIN, NEO_GRB + NEO_KHZ800);

struct LogChannel {
  const char *name;
  uint8_t key[CIPHER_KEY_SIZE];
  bool enabled;
};

LogChannel logChannels[] = {
  {"public", {0x8b, 0x33, 0x87, 0xe9, 0xc5, 0xcd, 0xea, 0x6a, 0xc9, 0xe5, 0xed, 0xba, 0xa1, 0x15, 0xcd, 0x72}, true},
  {IP_RELAY_PRIVATE_CHANNEL_1_NAME, {}, false},
  {IP_RELAY_PRIVATE_CHANNEL_2_NAME, {}, false},
  {IP_RELAY_PRIVATE_CHANNEL_3_NAME, {}, false},
  {IP_RELAY_PRIVATE_CHANNEL_4_NAME, {}, false}
};

void loadPrivateChannel(LogChannel &channel, const char *key) {
  if (channel.name[0] == 0 || strlen(key) != CIPHER_KEY_SIZE * 2) return;
  channel.enabled = mesh::Utils::fromHex(channel.key, CIPHER_KEY_SIZE, key);
}

void initLogChannels() {
  loadPrivateChannel(logChannels[1], IP_RELAY_PRIVATE_CHANNEL_1_KEY);
  loadPrivateChannel(logChannels[2], IP_RELAY_PRIVATE_CHANNEL_2_KEY);
  loadPrivateChannel(logChannels[3], IP_RELAY_PRIVATE_CHANNEL_3_KEY);
  loadPrivateChannel(logChannels[4], IP_RELAY_PRIVATE_CHANNEL_4_KEY);
}

void logGroupMessage(const uint8_t *raw, uint16_t rawLength) {
  mesh::Packet packet;
  if (!packet.readFrom(raw, rawLength) || packet.getPayloadType() != PAYLOAD_TYPE_GRP_TXT || packet.payload_len < 1 + CIPHER_MAC_SIZE + CIPHER_BLOCK_SIZE) return;

  const uint8_t *channelHash = packet.payload;
  const uint8_t *encrypted = packet.payload + 1;
  int encryptedLength = packet.payload_len - 1;
  uint8_t plain[MAX_PACKET_PAYLOAD];

  for (LogChannel &channel : logChannels) {
    if (!channel.enabled) continue;
    uint8_t expectedHash[PATH_HASH_SIZE];
    mesh::Utils::sha256(expectedHash, sizeof(expectedHash), channel.key, CIPHER_KEY_SIZE);
    if (channelHash[0] != expectedHash[0]) continue;

    int plainLength = mesh::Utils::MACThenDecrypt(channel.key, plain, encrypted, encryptedLength);
    if (plainLength < 6 || plain[4] != 0) return;
    size_t textLength = plainLength - 5;
    if (textLength >= sizeof(plain) - 5) textLength = sizeof(plain) - 6;
    plain[5 + textLength] = 0;
    Serial.printf("%s: %s\r\n", channel.name, (char *)&plain[5]);
    return;
  }
}

void updateStatusLed() {
  bool hasDhcpAddress = WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
  statusLed.setPixelColor(0, hasDhcpAddress ? statusLed.Color(0, 32, 0) : 0);
  statusLed.show();
}

void printWifiStatus() {
  Serial.print("WiFi SSID: ");
  Serial.println(wifiSsid);
  Serial.print("WiFi status: ");
  Serial.println(WiFi.status() == WL_CONNECTED ? "connected" : "disconnected");
  if (WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi IP: ");
    Serial.println(WiFi.localIP());
    Serial.print("WiFi gateway: ");
    Serial.println(WiFi.gatewayIP());
  }
}

void connectWifi() {
  WiFi.disconnect();
  WiFi.begin(wifiSsid.c_str(), wifiPassword.c_str());
  nextWifiRetryAt = millis() + WIFI_RETRY_INTERVAL_MS;
  Serial.println("WiFi connecting...");
}

void loadWifiSettings() {
  wifiPreferences.begin("ip-relay", false);
  wifiSsid = wifiPreferences.getString("ssid", WIFI_SSID);
  wifiPassword = wifiPreferences.getString("password", WIFI_PWD);
}

void handleSerialCommand(char *command) {
  char *argument = strchr(command, ' ');
  if (argument) {
    *argument++ = 0;
    while (*argument == ' ') argument++;
  }

  if (strcmp(command, "wifi") != 0 || !argument || !*argument) {
    Serial.println("Commands: wifi status | wifi ssid <name> | wifi password <password> | wifi connect | wifi clear");
    return;
  }

  char *value = strchr(argument, ' ');
  if (value) {
    *value++ = 0;
    while (*value == ' ') value++;
  }

  if (strcmp(argument, "status") == 0) {
    printWifiStatus();
  } else if (strcmp(argument, "ssid") == 0 && value && *value) {
    wifiSsid = value;
    wifiPreferences.putString("ssid", wifiSsid);
    Serial.println("WiFi SSID saved");
    connectWifi();
  } else if (strcmp(argument, "password") == 0 && value && *value) {
    wifiPassword = value;
    wifiPreferences.putString("password", wifiPassword);
    Serial.println("WiFi password saved");
    connectWifi();
  } else if (strcmp(argument, "connect") == 0) {
    connectWifi();
  } else if (strcmp(argument, "clear") == 0) {
    wifiPreferences.clear();
    wifiSsid = WIFI_SSID;
    wifiPassword = WIFI_PWD;
    Serial.println("Saved WiFi settings cleared");
    connectWifi();
  } else {
    Serial.println("Invalid WiFi command");
  }
  Serial.print("> ");
}

void handleSerial() {
  while (Serial.available()) {
    char character = Serial.read();
    if (character == '\r' || character == '\n') {
      if (serialCommandLength > 0) {
        Serial.println();
        serialCommand[serialCommandLength] = 0;
        handleSerialCommand(serialCommand);
        serialCommandLength = 0;
      }
    } else if (serialCommandLength < sizeof(serialCommand) - 1) {
      serialCommand[serialCommandLength++] = character;
      Serial.write(character);
    }
  }
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

void dumpReceivedFrame(const RelayClient &relayClient, int sourceIndex) {
#if BRIDGE_DEBUG
  Serial.printf("IP relay RX client %d, %u bytes:", sourceIndex, relayClient.framePosition);
  for (uint16_t index = 0; index < relayClient.framePosition; index++) {
    Serial.printf(" %02X", relayClient.frame[index]);
  }
  Serial.println();
#endif
}

void broadcastFrame(const uint8_t *frame, uint16_t frameLength, int sourceIndex) {
  for (int clientIndex = 0; clientIndex < IP_RELAY_MAX_CLIENTS; clientIndex++) {
    if (clientIndex == sourceIndex || !relayClients[clientIndex].socket.connected()) continue;
    relayClients[clientIndex].socket.write(frame, frameLength);
  }
}

bool rememberPacket(const uint8_t *raw, uint16_t rawLength) {
  mesh::Packet packet;
  uint8_t hash[MAX_HASH_SIZE];
  if (!packet.readFrom(raw, rawLength)) return false;
  packet.calculatePacketHash(hash);

  for (size_t index = 0; index < IP_RELAY_SEEN_CACHE_SIZE; index++) {
    if (memcmp(seenPacketHashes[index], hash, MAX_HASH_SIZE) == 0) return false;
  }

  memcpy(seenPacketHashes[nextSeenPacketHash], hash, MAX_HASH_SIZE);
  nextSeenPacketHash = (nextSeenPacketHash + 1) % IP_RELAY_SEEN_CACHE_SIZE;
  return true;
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
  dumpReceivedFrame(relayClient, sourceIndex);
  if (fletcher16(relayClient.frame + 4, payloadLength) == receivedChecksum) {
    if (rememberPacket(relayClient.frame + 4, payloadLength)) {
      logGroupMessage(relayClient.frame + 4, payloadLength);
      broadcastFrame(relayClient.frame, relayClient.framePosition, sourceIndex);
    }
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
  initLogChannels();
  loadWifiSettings();
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  connectWifi();
  updateStatusLed();

  relayServer.begin();
  Serial.printf("Relay listening on TCP port %d\r\n", IP_RELAY_PORT);
  Serial.print("> ");
}

void loop() {
  handleSerial();
  if (WiFi.status() != WL_CONNECTED) {
    if ((long)(millis() - nextWifiRetryAt) >= 0) connectWifi();
  } else {
    nextWifiRetryAt = millis() + WIFI_RETRY_INTERVAL_MS;
  }
  updateStatusLed();
  acceptClients();
  maintainClients();
  delay(1);
}
