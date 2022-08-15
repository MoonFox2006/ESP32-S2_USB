#include <stdio.h>
#include <Arduino.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <ArduinoJson.h>
#include <USB.h>
#include "USBFlash.h"

constexpr uint8_t LED_PIN = 15;
constexpr bool LED_LEVEL = HIGH;
constexpr uint32_t LED_PULSE = 25;

const char PARAM_WIFI_SSID[] = "wifi_ssid";
const char PARAM_WIFI_PSWD[] = "wifi_pswd";
const char PARAM_NTP_SERVER[] = "ntp_server";
const char PARAM_NTP_TZ[] = "ntp_tz";

struct JsonFileReader {
  JsonFileReader(FILE *f) : _f(f) {}

  // Reads one byte, or returns -1
  int read() {
    return fgetc(_f);
  }
  // Reads several bytes, returns the number of bytes read.
  size_t readBytes(char* buffer, size_t length) {
    return fread(buffer, 1, length, _f);
  }

  FILE *_f;
};

struct JsonFileWriter {
  JsonFileWriter(FILE *f) : _f(f) {}

  // Writes one byte, returns the number of bytes written (0 or 1)
  size_t write(uint8_t c) {
    return fputc(c, _f) == c;
  }
  // Writes several bytes, returns the number of bytes written
  size_t write(const uint8_t *buffer, size_t length) {
    return fwrite(buffer, 1, length, _f);
  }

  FILE *_f;
};

USBCDC serial;
USBFlash flash;
StaticJsonDocument<256> config;

static void halt(const char *msg) {
  serial.println(msg);
  serial.flush();
  serial.end();
  esp_deep_sleep_start();
}

static void resetConfig() {
  config[PARAM_WIFI_SSID] = "";
  config[PARAM_WIFI_PSWD] = "";
  config[PARAM_NTP_SERVER] = "pool.ntp.org";
  config[PARAM_NTP_TZ] = 3;
}

static bool readConfig(const char *fileName) {
  FILE *f;
  bool result = false;

  f = fopen(fileName, "r");
  if (f) {
    JsonFileReader reader(f);

    if (deserializeJson(config, reader) == DeserializationError::Ok)
      result = true;
    else
      resetConfig();
    fclose(f);
  } else {
    resetConfig();
    f = fopen(fileName, "w");
    if (f) {
      JsonFileWriter writer(f);

      serializeJson(config, writer);
      fclose(f);
    }
  }
  return result;
}

static bool wifiConnect(const char *ssid, const char *pswd, uint32_t timeout = 30000) {
  uint32_t time;

  serial.printf("Connecting to WiFi \"%s\"", ssid);
  WiFi.begin(ssid, pswd);
  time = millis();
  while ((! WiFi.isConnected()) && (millis() - time < timeout)) {
    digitalWrite(LED_PIN, LED_LEVEL);
    delay(LED_PULSE);
    digitalWrite(LED_PIN, ! LED_LEVEL);
    delay(500 - LED_PULSE);
    serial.print('.');
  }
  if (WiFi.isConnected()) {
    serial.print(" OK (IP: ");
    serial.print(WiFi.localIP());
    serial.println(')');
    return true;
  } else {
    WiFi.disconnect();
    serial.println(" FAIL!");
    return false;
  }
}

bool ntpUpdate(const char *ntp_server, int8_t tz, uint32_t timeout = 1000, uint8_t repeat = 1) {
  const uint16_t LOCAL_PORT = 55123;

  if (WiFi.isConnected()) {
    WiFiUDP udp;

    if (udp.begin(LOCAL_PORT)) {
      do {
        uint8_t buffer[48];

        memset(buffer, 0, sizeof(buffer));
        // Initialize values needed to form NTP request
        buffer[0] = 0B11100011; // LI, Version, Mode
        buffer[1] = 0; // Stratum, or type of clock
        buffer[2] = 6; // Polling Interval
        buffer[3] = 0xEC; // Peer Clock Precision
        // 8 bytes of zero for Root Delay & Root Dispersion
        buffer[12] = 49;
        buffer[13] = 0x4E;
        buffer[14] = 49;
        buffer[15] = 52;
        // all NTP fields have been given values, now
        // you can send a packet requesting a timestamp
        if (udp.beginPacket(ntp_server, 123) && (udp.write(buffer, sizeof(buffer)) == sizeof(buffer)) && udp.endPacket()) {
          uint32_t time = millis();
          int cb;

          while ((! (cb = udp.parsePacket())) && (millis() - time < timeout)) {
            delay(1);
          }
          if (cb) {
            time = millis() - time;
            // We've received a packet, read the data from it
            if (udp.read(buffer, sizeof(buffer)) == sizeof(buffer)) { // read the packet into the buffer
              timeval val;

              // the timestamp starts at byte 40 of the received packet and is four bytes,
              // or two words, long. First, esxtract the two words:
              val.tv_sec = (((uint32_t)buffer[40] << 24) | ((uint32_t)buffer[41] << 16) | ((uint32_t)buffer[42] << 8) | buffer[43]) - 2208988800UL;
              val.tv_sec += tz * 3600;
              val.tv_sec += time / 1000;
              val.tv_usec = (time % 1000) * 1000;
              settimeofday(&val, nullptr);
              return true;
            }
          }
        }
      } while (repeat--);
    }
  }
  return false;
}

void setup() {
  pinMode(LED_PIN, OUTPUT);
  digitalWrite(LED_PIN, ! LED_LEVEL);

  USB.begin();
  serial.begin(115200);
  delay(2000);
  if ((! flash.init()) || (! flash.begin()))
    halt("USB MSC init fail!");

  if (! readConfig("/fatfs/Config.json"))
    serial.println("Bad config file! Use default configuration.");
  serial.printf("\"%s\": \"%s\"\n", PARAM_WIFI_SSID, config[PARAM_WIFI_SSID] | "");
  serial.printf("\"%s\": \"%s\"\n", PARAM_WIFI_PSWD, config[PARAM_WIFI_PSWD] | "");
  serial.printf("\"%s\": \"%s\"\n", PARAM_NTP_SERVER, config[PARAM_NTP_SERVER] | "pool.ntp.org");
  serial.printf("\"%s\": %d\n", PARAM_NTP_TZ, config[PARAM_NTP_TZ] | 3);

  if ((! config[PARAM_WIFI_SSID].isNull()) && *config[PARAM_WIFI_SSID].as<const char*>()) {
    WiFi.persistent(false);
    WiFi.mode(WIFI_STA);
    if (wifiConnect(config[PARAM_WIFI_SSID], config[PARAM_WIFI_PSWD])) {
      if (ntpUpdate(config[PARAM_NTP_SERVER] | "pool.ntp.org", config[PARAM_NTP_TZ] | 3)) {
        timeval tm;

        gettimeofday(&tm, nullptr);
        tm.tv_sec %= (3600 * 24);
        serial.printf("NTP time: %02u:%02u:%02u\n", tm.tv_sec / 3600, (tm.tv_sec % 3600) / 60, tm.tv_sec % 60);
      } else
        serial.println("Getting NTP time fail!");
    }
  }
}

void loop() {
  serial.println(millis() / 1000);
  digitalWrite(LED_PIN, LED_LEVEL);
  delay(LED_PULSE);
  digitalWrite(LED_PIN, ! LED_LEVEL);
  delay(1000 - LED_PULSE);
}
