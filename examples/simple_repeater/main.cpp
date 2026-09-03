#include <Arduino.h>   // needed for PlatformIO
#include <Mesh.h>

#include "MyMesh.h"

#ifdef DISPLAY_CLASS
  #include "UITask.h"
  static UITask ui_task(board, display);
#endif

#ifdef ETHERNET_ENABLED
  #define ETHERNET_CLI_BANNER "MeshCore Repeater CLI"
  #include <helpers/nrf52/EthernetCLI.h>
#endif

#ifdef WIFI_SSID
  #include <WiFi.h>
  #ifndef WIFI_CLI_PORT
    #define WIFI_CLI_PORT 2323
  #endif
  static WiFiServer wifi_cli_server(WIFI_CLI_PORT);
  static WiFiClient wifi_cli_client;
  static char wifi_cli_command[160];
  static bool wifi_ip_reported = false;

  static void wifi_event_handler(WiFiEvent_t event) {
    if (event == ARDUINO_EVENT_WIFI_STA_GOT_IP) {
      Serial.print("WiFi connected, IP: ");
      Serial.println(WiFi.localIP());
      wifi_ip_reported = true;
    }
  }
#endif

StdRNG fast_rng;
SimpleMeshTables tables;
static bool radio_available = false;

MyMesh the_mesh(board, radio_driver, *new ArduinoMillis(), fast_rng, rtc_clock, tables);

void halt() {
  while (1) ;
}

static char command[160];
#ifdef ETHERNET_ENABLED
static char ethernet_command[160];
#endif

static const char CLI_PROMPT[] = "MeshCore> ";

#ifdef WIFI_SSID
static void wifi_cli_start() {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.onEvent(wifi_event_handler);
  WiFi.begin(WIFI_SSID, WIFI_PWD);
  wifi_cli_server.begin();
  MESH_DEBUG_PRINTLN("WiFi CLI listening on TCP port %d", WIFI_CLI_PORT);
}

static void wifi_cli_loop() {
  if (!wifi_ip_reported && WiFi.status() == WL_CONNECTED) {
    Serial.print("WiFi connected, IP: ");
    Serial.println(WiFi.localIP());
    wifi_ip_reported = true;
  }

  WiFiClient incoming = wifi_cli_server.accept();
  if (incoming) {
    wifi_cli_client.stop();
    wifi_cli_client = incoming;
    wifi_cli_command[0] = 0;
    wifi_cli_client.println("MeshCore Repeater CLI");
    wifi_cli_client.print(CLI_PROMPT);
  }

  if (!wifi_cli_client || !wifi_cli_client.connected()) return;

  size_t len = strlen(wifi_cli_command);
  while (wifi_cli_client.available() && len < sizeof(wifi_cli_command) - 1) {
    char c = wifi_cli_client.read();
    if (c == '\n' && len == 0) continue;
    if (c == '\r' || c == '\n') {
      char reply[160] = {0};
      the_mesh.handleCommand(0, wifi_cli_command, reply);
      if (reply[0]) wifi_cli_client.println(reply);
      wifi_cli_command[0] = 0;
      len = 0;
      wifi_cli_client.print(CLI_PROMPT);
    } else {
      wifi_cli_command[len++] = c;
      wifi_cli_command[len] = 0;
    }
  }
}
#endif

// For power saving
unsigned long POWERSAVING_FIRSTSLEEP_SECS = 120; // The first sleep (if enabled) from boot

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_)
static unsigned long userBtnDownAt = 0;
#define USER_BTN_HOLD_OFF_MILLIS 1500
#endif

void setup() {
  Serial.begin(115200);
  delay(1000);
  Serial.println("MeshCore: setup");

  board.begin();
  Serial.println("MeshCore: board ready");

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.begin();
#endif

#if defined(MESH_DEBUG) && defined(NRF52_PLATFORM)
  // give some extra time for serial to settle so
  // boot debug messages can be seen on terminal
  delay(5000);
#endif

#ifdef DISPLAY_CLASS
  if (display.begin()) {
    display.startFrame();
    display.setCursor(0, 0);
    display.print("Please wait...");
    display.endFrame();
  }
#endif

#ifdef WIFI_SSID
  wifi_cli_start();
  Serial.println("MeshCore: WiFi starting");
#endif

  radio_available = radio_init();
  if (!radio_available) {
    Serial.println("MeshCore: radio unavailable, continuing without LoRa");
  } else {
    Serial.println("MeshCore: radio ready");
  }

  fast_rng.begin(radio_available ? radio_driver.getRngSeed() : micros());

  FILESYSTEM* fs;
#if defined(NRF52_PLATFORM) || defined(STM32_PLATFORM)
  InternalFS.begin();
  fs = &InternalFS;
  IdentityStore store(InternalFS, "");
#elif defined(ESP32)
  SPIFFS.begin(true);
  fs = &SPIFFS;
  IdentityStore store(SPIFFS, "/identity");
#elif defined(RP2040_PLATFORM)
  LittleFS.begin();
  fs = &LittleFS;
  IdentityStore store(LittleFS, "/identity");
  store.begin();
#else
  #error "need to define filesystem"
#endif
  if (!store.load("_main", the_mesh.self_id)) {
    MESH_DEBUG_PRINTLN("Generating new keypair");
    the_mesh.self_id = radio_available ? radio_new_identity() : mesh::LocalIdentity(&fast_rng);
    int count = 0;
    while (count < 10 && (the_mesh.self_id.pub_key[0] == 0x00 || the_mesh.self_id.pub_key[0] == 0xFF)) {  // reserved id hashes
      the_mesh.self_id = radio_available ? radio_new_identity() : mesh::LocalIdentity(&fast_rng); count++;
    }
    store.save("_main", the_mesh.self_id);
  }

  Serial.print("Repeater ID: ");
  mesh::Utils::printHex(Serial, the_mesh.self_id.pub_key, PUB_KEY_SIZE); Serial.println();

  command[0] = 0;
#ifdef ETHERNET_ENABLED
  ethernet_command[0] = 0;
#endif

  Serial.print(CLI_PROMPT);

  sensors.begin();

  the_mesh.begin(fs);
  Serial.println("MeshCore: mesh ready");

#ifdef DISPLAY_CLASS
  ui_task.begin(the_mesh.getNodePrefs(), FIRMWARE_BUILD_DATE, FIRMWARE_VERSION);
#endif

#ifdef ETHERNET_ENABLED
  ethernet_start_task();
#endif

  // send out initial zero hop Advertisement to the mesh
#if ENABLE_ADVERT_ON_BOOT == 1
  if (radio_available) the_mesh.sendSelfAdvertisement(16000, false);
#endif

  board.onBootComplete();
  Serial.println("MeshCore: ready");
}

void loop() {
  // Handle Serial CLI
  int len = strlen(command);
  bool serial_line_complete = false;
  while (Serial.available() && len < sizeof(command)-1) {
    char c = Serial.read();
    if (c != '\r' && c != '\n') {
      command[len++] = c;
      command[len] = 0;
      Serial.print(c);
    }
    if (c == '\r' || c == '\n') {
      serial_line_complete = true;
      break;
    }
  }
  if (len == sizeof(command)-1) {  // command buffer full
    command[sizeof(command)-1] = '\r';
  }

  if (serial_line_complete && Serial.available() && (Serial.peek() == '\r' || Serial.peek() == '\n')) {
    Serial.read();  // consume the second character of CRLF
  }

  if (len == sizeof(command)-1 || serial_line_complete) {  // received complete line
    Serial.print('\n');
    command[len] = 0;
    char reply[160];
    reply[0] = 0;
#ifdef ETHERNET_ENABLED
    if (!ethernet_handle_command(command, reply)) {
      the_mesh.handleCommand(0, command, reply);
    }
#else
    the_mesh.handleCommand(0, command, reply);  // NOTE: there is no sender_timestamp via serial!
#endif
    if (reply[0]) {
      Serial.print("  -> "); Serial.println(reply);
    }

      command[0] = 0;  // reset command buffer
      Serial.print(CLI_PROMPT);  // Show prompt after command
  }

#ifdef ETHERNET_ENABLED
  ethernet_loop_maintain();
  if (ethernet_read_line(ethernet_command, sizeof(ethernet_command))) {
    char reply[160];
    reply[0] = 0;
    if (!ethernet_handle_command(ethernet_command, reply)) {
      the_mesh.handleCommand(0, ethernet_command, reply);
    }
    ethernet_send_reply(reply);
    ethernet_command[0] = 0;
  }
#endif

#ifdef WIFI_SSID
  wifi_cli_loop();
#endif

#if defined(PIN_USER_BTN) && defined(_SEEED_SENSECAP_SOLAR_H_) && !defined(DISPLAY_CLASS)
  // Hold the user button to power off the SenseCAP Solar repeater.
  int btnState = digitalRead(PIN_USER_BTN);
  if (btnState == LOW) {
    if (userBtnDownAt == 0) {
      userBtnDownAt = millis();
    } else if ((unsigned long)(millis() - userBtnDownAt) >= USER_BTN_HOLD_OFF_MILLIS) {
      Serial.println("Powering off...");
      board.powerOff();  // does not return
    }
  } else {
    userBtnDownAt = 0;
  }
#endif

  if (radio_available) the_mesh.loop();
  sensors.loop();
#ifdef DISPLAY_CLASS
  ui_task.loop();
#endif
  rtc_clock.tick();

#ifdef HAS_EXTERNAL_WATCHDOG
  external_watchdog.loop();
#endif
  if (the_mesh.getNodePrefs()->powersaving_enabled && !the_mesh.hasPendingWork()) {
#if defined(NRF52_PLATFORM)
    board.sleep(0); // nrf ignores seconds param, sleeps whenever possible
#else
    if (the_mesh.millisHasNowPassed(POWERSAVING_FIRSTSLEEP_SECS * 1000)) { // To check if it is time to sleep
      board.sleep(30); // Sleep. Wake up after a while or when receiving a LoRa packet
    }
#endif
  }
}
