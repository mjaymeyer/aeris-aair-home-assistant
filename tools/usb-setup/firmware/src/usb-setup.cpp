// Temporary USB helper: reads/writes the Aeris app's EEPROM settings over USB serial so a unit can be
// set up without the setup hotspot. Driven by ../provision.mjs; flash the real firmware right after.
// Commands: READ -> 352-byte Settings + CRC32; WRITE + Settings + CRC32 -> OK; NET -> Wi-Fi diagnostics
// (no READ after NET without a reset - the periodic status lines would corrupt the framing).
#include "Particle.h"
#include <stddef.h>

SYSTEM_MODE(MANUAL);
SYSTEM_THREAD(ENABLED);

// Match the existing Aeris application, including compiler padding.
struct Settings {
    char build_signature[25];
    char wifi_ssid[64];
    char wifi_pass[64];
    char mqtt_ip[32];
    int mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[32];
    char topic_prefix[64];
    int fanFontSize, fanX, fanY, pmFontSize, pmX, pmY;
    uint16_t fanColor, pmLabelColor, pmValueColor;
};
static_assert(sizeof(Settings) == 352, "Aeris EEPROM size mismatch");
static_assert(offsetof(Settings, wifi_ssid) == 25, "SSID offset mismatch");
static_assert(offsetof(Settings, wifi_pass) == 89, "Password offset mismatch");
static_assert(offsetof(Settings, mqtt_ip) == 153, "Broker offset mismatch");
static_assert(offsetof(Settings, mqtt_port) == 188, "Port offset mismatch");
static_assert(offsetof(Settings, topic_prefix) == 256, "Topic offset mismatch");

bool networkTest = false;
unsigned long lastStatus = 0;
bool tcpTested = false;

uint32_t checksum(const void* data, size_t size) {
    const uint8_t* p = static_cast<const uint8_t*>(data);
    uint32_t crc = 0xffffffff;
    while (size--) {
        crc ^= *p++;
        for (int i = 0; i < 8; i++) {
            crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320 : 0);
        }
    }
    return ~crc;
}

bool valid(const Settings& s) {
    return s.build_signature[0] && memchr(s.build_signature, 0, sizeof(s.build_signature)) &&
        s.wifi_ssid[0] && memchr(s.wifi_ssid, 0, sizeof(s.wifi_ssid)) &&
        memchr(s.wifi_pass, 0, sizeof(s.wifi_pass)) &&
        memchr(s.mqtt_ip, 0, sizeof(s.mqtt_ip)) &&   // empty = Wi-Fi only, as the firmware allows
        s.mqtt_port > 0 && s.mqtt_port <= 65535 &&
        memchr(s.mqtt_user, 0, sizeof(s.mqtt_user)) &&
        memchr(s.mqtt_pass, 0, sizeof(s.mqtt_pass)) &&
        s.topic_prefix[0] && memchr(s.topic_prefix, 0, sizeof(s.topic_prefix));
}

void setup() {
    // Keep the helper offline and leave all application GPIOs alone.
    WiFi.off();
    Serial.begin(115200);
    Serial.setTimeout(10000);
}

void loop() {
    static char command[8] = {};
    static size_t used = 0;
    while (Serial.available()) {
        char c = Serial.read();
        if (c != '\n') {
            if (used < sizeof(command) - 1) command[used++] = c;
            continue;
        }
        command[used] = 0;
        used = 0;
        if (!strcmp(command, "READ")) {
            Settings s;
            EEPROM.get(0, s);
            uint32_t crc = checksum(&s, sizeof(s));
            Serial.write(reinterpret_cast<const uint8_t*>(&s), sizeof(s));
            Serial.write(reinterpret_cast<const uint8_t*>(&crc), sizeof(crc));
        } else if (!strcmp(command, "WRITE")) {
            Settings s;
            uint32_t crc;
            if (Serial.readBytes(reinterpret_cast<char*>(&s), sizeof(s)) != sizeof(s) ||
                Serial.readBytes(reinterpret_cast<char*>(&crc), sizeof(crc)) != sizeof(crc)) {
                Serial.println("ERR incomplete");
            } else if (crc != checksum(&s, sizeof(s)) || !valid(s)) {
                Serial.println("ERR invalid");
            } else {
                EEPROM.put(0, s);
                Settings verified;
                EEPROM.get(0, verified);
                Serial.println(memcmp(&s, &verified, sizeof(s)) == 0 ? "OK" : "ERR verify");
            }
        } else if (!strcmp(command, "NET")) {
            Settings s;
            EEPROM.get(0, s);
            if (!valid(s)) {
                Serial.println("ERR settings");
                continue;
            }
            networkTest = false;
            WiFi.on();
            unsigned long began = millis();
            while (!WiFi.isOn() && millis() - began < 20000) delay(100);
            Serial.printf("NET radio_on=%d elapsed_ms=%lu\n", WiFi.isOn(), millis() - began);
            if (!WiFi.isOn()) {
                Serial.println("ERR radio startup");
                continue;
            }
            WiFiAccessPoint aps[30];
            int count = WiFi.scan(aps, 30);
            int matches = 0;
            for (int i = 0; i < count; i++) {
                if (!strcmp(aps[i].ssid, s.wifi_ssid)) {
                    matches++;
                    Serial.printf("NET target channel=%d rssi=%d security=%d\n", aps[i].channel, aps[i].rssi, aps[i].security);
                }
            }
            Serial.printf("NET scan count=%d target_matches=%d\n", count, matches);
            bool stored = WiFi.setCredentials(s.wifi_ssid, s.wifi_pass);
            Serial.printf("NET credentials stored=%d\n", stored);
            if (!stored) {
                Serial.println("ERR credentials");
                continue;
            }
            networkTest = true;
            tcpTested = false;
            WiFi.connect();
        } else {
            Serial.println("ERR command");
        }
    }
    if (networkTest && millis() - lastStatus >= 5000) {
        lastStatus = millis();
        Serial.printf("NET ready=%d connecting=%d listening=%d ip=%s\n", WiFi.ready(), WiFi.connecting(), WiFi.listening(), WiFi.localIP().toString().c_str());
        if (WiFi.ready() && !tcpTested) {
            Settings s;
            EEPROM.get(0, s);
            Serial.printf("NET target_connected=%d\n", String(WiFi.SSID()) == String(s.wifi_ssid));
            TCPClient tcp;
            if (s.mqtt_ip[0]) Serial.printf("NET broker TCP=%d\n", tcp.connect(s.mqtt_ip, s.mqtt_port));
            tcp.stop();
            tcpTested = true;
        }
    }
}
