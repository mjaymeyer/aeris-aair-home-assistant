#include <MQTT.h>
#include "Adafruit_ILI9341.h"
#include "Adafruit_mfGFX.h"
#include "softap_http.h"
#include "SparkIntervalTimer.h"

// -------------------------------------------------------------------------
// 1. CONFIGURATION
// -------------------------------------------------------------------------
#define EEPROM_ADDR 0

// Always updating string so each compile will reset unit back to initial setup
const char* CURRENT_BUILD_SIG = __DATE__ " " __TIME__;

struct Settings {
    char build_signature[25]; // Stores the build timestamp
    
    char wifi_ssid[64];
    char wifi_pass[64];
    char mqtt_ip[32];
    int  mqtt_port;
    char mqtt_user[32];
    char mqtt_pass[32];
    char topic_prefix[64];
    
    // Display Coordinates/Size
    int fanFontSize;
    int fanX;
    int fanY;
    int pmFontSize;
    int pmX;
    int pmY;

    // Display Colors
    uint16_t fanColor;
    uint16_t pmLabelColor;
    uint16_t pmValueColor;
};

Settings mySettings;

// -------------------------------------------------------------------------
// COLOR DEFINITIONS
// -------------------------------------------------------------------------
const int NUM_COLORS = 19;
const char* colorNames[] = {
    "BLACK", "NAVY", "DARKGREEN", "DARKCYAN", "MAROON", "PURPLE", "OLIVE",
    "LIGHTGREY", "DARKGREY", "BLUE", "GREEN", "CYAN", "RED", "MAGENTA",
    "YELLOW", "WHITE", "ORANGE", "GREENYELLOW", "PINK"
};
const uint16_t colorValues[] = {
    ILI9341_BLACK, ILI9341_NAVY, ILI9341_DARKGREEN, ILI9341_DARKCYAN, ILI9341_MAROON, ILI9341_PURPLE, ILI9341_OLIVE,
    ILI9341_LIGHTGREY, ILI9341_DARKGREY, ILI9341_BLUE, ILI9341_GREEN, ILI9341_CYAN, ILI9341_RED, ILI9341_MAGENTA,
    ILI9341_YELLOW, ILI9341_WHITE, ILI9341_ORANGE, ILI9341_GREENYELLOW, ILI9341_PINK
};

// -------------------------------------------------------------------------
// 2. HARDWARE
// -------------------------------------------------------------------------
const int PIN_FAN       = D0;  
const int PIN_SENSOR_TX = A6;  
#define TFT_CS   A2
#define TFT_DC   A0
#define TFT_RST  -1
Adafruit_ILI9341 tft = Adafruit_ILI9341(TFT_CS, TFT_DC, TFT_RST);

const int PIN_DISP_BL   = A1; 
// Ring and button lights sit behind a 74HC595 shift register: D5 data, D6 clock, D7 latch, LSB first,
// plus a boot handshake pulse on A4. Unlatched, it shows random power-up contents. Bits: 0 ring boost,
// 1 all buttons dim, 2 power, 3 A, 4 minus, 5 plus, 6 ring, 7 unused.
const int PIN_RING_DATA = D5;
const int PIN_RING_CLK  = D6;
const int PIN_RING_LATCH = D7;
const int PIN_RING_HS   = A4;
// LED channels 0 ring, 1 buttons, set over MQTT JSON or the web page: on, brightness 0-255, effect, follow the
// display, which buttons (mask 1 power, 2 A, 4 minus, 8 plus) and effect speed 10-250% (100% = 0.7x the base timings).
// Saved at EEPROM 1024, outside Settings.
// Effects 0-4 suit both channels; 5-9 need several lights (buttons only).
const char* LED_FX[] = {"solid", "blink", "breathe", "pulse", "candle", "chase", "bounce", "fill", "wave", "twinkle"};
const int LED_FX_N = 10, LED_ADDR = 1024;
struct LedPrefs { uint8_t magic, on[2], bright[2], fx[2], follow[2], mask, speed[2], sum; };
LedPrefs lp; volatile uint8_t ledDuty[8]; uint8_t ledGamma[256], ledLum[256], ledHi[2];
unsigned long ledDirty = 0; bool ledSave = false, ledPublish = false;
const int BTN_UP        = D1;
const int BTN_DOWN      = D2; 
const int BTN_EXTRA     = D3;
const int BTN_POWER     = D4;

// -------------------------------------------------------------------------
// GLOBALS
// -------------------------------------------------------------------------
SYSTEM_MODE(SEMI_AUTOMATIC);
SYSTEM_THREAD(ENABLED);

TCPServer webServer(80);
MQTT *client = NULL;
String mqttClientId;

// Fan PWM for 0-100 %: equal airflow steps from the slowest steady speed (1 %) to full, measured by microphone
// (PWM 17-26 all run at ~22 % of full speed; the fan stops below ~16 and needs 22 to start from standstill).
const uint8_t FAN_PWM[101] = {
    0, 26, 27, 27, 28, 28, 29, 29, 30, 30, 30, 31, 31, 31, 32, 32, 33, 33, 33, 34, 34,
    36, 36, 38, 39, 40, 41, 42, 43, 44, 45, 45, 46, 47, 48, 49, 51, 52, 54, 55, 57, 58,
    60, 61, 63, 65, 66, 68, 71, 74, 76, 77, 79, 81, 84, 86, 88, 90, 92, 95, 97, 100, 102,
    105, 108, 110, 113, 116, 119, 122, 125, 128, 131, 134, 138, 141, 144, 148, 152, 155, 159, 163, 167, 171,
    175, 179, 184, 188, 192, 197, 202, 206, 211, 216, 221, 226, 231, 236, 241, 247, 255};
unsigned long fanKickAt = 0; bool fanKick = false;   // a start from standstill gets 0.5 s at PWM 64 or more
// Speed step in % (1-25, default 1) for the buttons, MQTT and the web page, and the fan state to resume after a
// reboot (speed, 0 = off, and the last running speed): saved at EEPROM 1088, outside Settings, 3 s after a change.
const int FAN_ADDR = 1088; struct FanPrefs { uint8_t magic, step, speed, saved, sum; } fp;
unsigned long fanDirtyAt = 0; bool fanSave = false;
bool fanHeld = false, fanStepPublish = false;   // fanHeld: + / - held down, MQTT updates wait for the release
unsigned long btnRepAt[4]; int btnReps[4];
int currentSpeed = 0;      
int savedSpeed   = 25;     
bool lightsOn    = true;   
String lastPM25 = "0";
String lastPM10 = "0";
int drawnSpeed = -999;     
String drawnPM25 = "-999"; 
int lastBtnState[] = {1,1,1,1};   // released: the panel's button lines are active-low (high at rest)
byte rxBuffer[128];        
int rxIndex = 0;
unsigned long lastRxTime = 0; 
const byte CMD_HPMA_START[] = {0x68, 0x01, 0x01, 0x96}; 
const byte CMD_PMS_WAKE[]   = {0x42, 0x4D, 0xE4, 0x00, 0x01, 0x01, 0x74}; 
const byte CMD_PMS_ACTIVE[] = {0x42, 0x4D, 0xE1, 0x00, 0x01, 0x01, 0x71}; 

// --- SMOOTHING VARIABLES FOR AQ SENSOR ---
const int WINDOW = 10;      // Size of the moving average window
int pm25Hist[WINDOW];       // Buffer for PM2.5 readings
int pm10Hist[WINDOW];       // Buffer for PM10 readings
int histIndex = 0;          // Current write position in the buffer
bool histFilled = false;    // Track if we have enough data for a full average
int zeroCount = 0;          // Counts consecutive zeros
// --------------------------------------------------

// Prototypes
void http_handler(const char* url, ResponseCallback* cb, void* cbArg, Reader* body, Writer* result, void* reserved);
void handleWebPortal(); 
void mqtt_callback(char* topic, byte* payload, unsigned int length);
void drawDynamicValues(); void updateSystem(); void checkButton(int pin, int idx); void handlePress(int pin);
void fanLoad(); void fanSetStep(int s); int fanSnap(int pct); uint8_t fanSum();
void initRing(); void writeRing(uint8_t v); void ledsTick(); void ledsPwm(); void ledSet(int c, String k, String v);
void ledsChanged(); void publishLeds(); uint8_t ledSum();
Timer ledTimer(1, ledsTick); IntervalTimer ledPwmTimer;
void sendRawByte(const byte* data, int len); void processSensorData(); void publishAll();
void enterSetupMode(); void enterNormalMode(); void loadDefaults();
void printColorSelect(TCPClient &c, String name, uint16_t selected);

STARTUP(System.set(SYSTEM_CONFIG_SOFTAP_PREFIX, "Aeris"));
STARTUP(softap_set_application_page_handler(http_handler, nullptr));

void setup() {
    Serial.begin(9600);
    mqttClientId = "Aeris-" + System.deviceID();
    
    pinMode(PIN_FAN, OUTPUT);
    pinMode(PIN_SENSOR_TX, OUTPUT); digitalWrite(PIN_SENSOR_TX, HIGH); 
    pinMode(PIN_DISP_BL, OUTPUT); initRing(); fanLoad(); ledTimer.start(); ledPwmTimer.begin(ledsPwm, 50, uSec, TIMER6);
    pinMode(BTN_UP, INPUT_PULLUP); pinMode(BTN_DOWN, INPUT_PULLUP);
    pinMode(BTN_EXTRA, INPUT_PULLUP); pinMode(BTN_POWER, INPUT_PULLUP);
    
    tft.begin(); tft.fillScreen(ILI9341_BLACK); tft.setRotation(3); 
    Serial1.begin(9600); RGB.control(true);

    EEPROM.get(EEPROM_ADDR, mySettings);
    
    // COMPARE TIMESTAMP:
    // If the signature in EEPROM doesn't match the signature of THIS binary,
    // it means we just flashed new code. Force a reset.
    if (strcmp(mySettings.build_signature, CURRENT_BUILD_SIG) != 0) {
        loadDefaults();
        enterSetupMode();
    } 
    else if (strlen(mySettings.wifi_ssid) == 0) {
        enterSetupMode();
    }
    else {
        enterNormalMode();
    }
}

void loadDefaults() {
    memset(&mySettings, 0, sizeof(mySettings));
    
    // Save the current binary's timestamp so we don't reset on next boot
    strncpy(mySettings.build_signature, CURRENT_BUILD_SIG, 24);
    
    strcpy(mySettings.mqtt_ip, ""); 
    mySettings.mqtt_port = 1883;
    strcpy(mySettings.mqtt_user, "mqtt_user");
    strcpy(mySettings.topic_prefix, "aeris/purifier/");
    
    mySettings.fanFontSize = 10;
    mySettings.fanX = 30;
    mySettings.fanY = 70;
    mySettings.pmFontSize = 3;
    mySettings.pmX = 60;
    mySettings.pmY = 180;

    // Color Defaults
    mySettings.fanColor = ILI9341_GREENYELLOW;
    mySettings.pmLabelColor = ILI9341_CYAN;
    mySettings.pmValueColor = ILI9341_WHITE;
    
    EEPROM.put(EEPROM_ADDR, mySettings);
}

void enterSetupMode() {
    RGB.color(0, 0, 255);
    tft.fillScreen(ILI9341_BLUE);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(3); tft.setCursor(10, 50);
    tft.println("SETUP MODE");
    tft.setTextSize(2);
    tft.setCursor(10, 100); tft.println("Connect to:");
    tft.setCursor(10, 130); tft.println("Aeris-XXXX");
    tft.setCursor(10, 180); tft.println("http://192.168.0.1");
    digitalWrite(PIN_DISP_BL, HIGH);
    
    WiFi.on();
    WiFi.listen(); 
}

void enterNormalMode() {
    RGB.color(255, 100, 0);
    tft.fillScreen(ILI9341_BLACK);
    tft.setTextColor(ILI9341_WHITE); tft.setTextSize(2); tft.setCursor(10, 100);
    tft.println("Connecting...");
    digitalWrite(PIN_DISP_BL, HIGH);

    WiFi.on();
    if(strlen(mySettings.wifi_pass) > 0) 
        WiFi.setCredentials(mySettings.wifi_ssid, mySettings.wifi_pass);
    else 
        WiFi.setCredentials(mySettings.wifi_ssid);
        
    WiFi.connect();
    webServer.begin(); 
    
    currentSpeed = fp.speed; savedSpeed = fp.saved ? fp.saved : 25; lightsOn = currentSpeed > 0;   // resume the saved fan state
    updateSystem();
    drawDynamicValues();
}

void loop() {
    if (fanSave && millis() - fanDirtyAt > 3000) { fanSave = false; fp.speed = currentSpeed; fp.saved = savedSpeed; fp.sum = fanSum(); EEPROM.put(FAN_ADDR, fp); }
    if (fanKick && millis() - fanKickAt > 500) { fanKick = false; analogWrite(PIN_FAN, FAN_PWM[constrain(currentSpeed, 0, 100)]); }
    if (WiFi.listening()) return; 

    handleWebPortal();
    
    static unsigned long lastConn = 0;
    if (!WiFi.ready()) {
        if(millis() - lastConn > 5000) { lastConn=millis(); WiFi.connect(); }
    } 
    else {
        if (client == NULL && strlen(mySettings.mqtt_ip) > 0) {
            client = new MQTT(mySettings.mqtt_ip, mySettings.mqtt_port, mqtt_callback);
        }
        if (client != NULL) {
            if (!client->isConnected()) {
                client->connect(mqttClientId.c_str(), mySettings.mqtt_user, mySettings.mqtt_pass);
                if (client->isConnected()) {
                    client->subscribe(String(mySettings.topic_prefix) + "fan/set");
                    client->subscribe(String(mySettings.topic_prefix) + "lights/set");
                    client->subscribe(String(mySettings.topic_prefix) + "ring/set");
                    client->subscribe(String(mySettings.topic_prefix) + "buttons/set");
                    client->subscribe(String(mySettings.topic_prefix) + "fan/step/set");
                    ledPublish = true; fanStepPublish = true;
                    RGB.color(0, 255, 255); 
                }
            } else {
                client->loop();
                if (ledPublish) { ledPublish = false; publishLeds(); }
                if (fanStepPublish) { fanStepPublish = false; client->publish((String(mySettings.topic_prefix) + "fan/step/state").c_str(), String(fp.step).c_str(), true); }
            }
        }
    }

    checkButton(BTN_POWER, 3); checkButton(BTN_UP, 0); checkButton(BTN_DOWN, 1); checkButton(BTN_EXTRA, 2);
    if (ledSave && millis() - ledDirty > 3000) { ledSave = false; lp.sum = ledSum(); EEPROM.put(LED_ADDR, lp); }

    // --- SENSOR WATCHDOG ---
    // Only send wake commands if we haven't heard from the sensor for 10 seconds.
    // This prevents "reset spikes" while ensuring the sensor stays alive.
    if (millis() - lastRxTime > 10000) {
        // Reset the RX timer so we don't spam commands instantly
        lastRxTime = millis(); 
        
        Serial.println("Sensor Watchdog: Waking sensor...");
        sendRawByte(CMD_HPMA_START, sizeof(CMD_HPMA_START)); delay(50);
        sendRawByte(CMD_PMS_WAKE, sizeof(CMD_PMS_WAKE)); delay(50);
        sendRawByte(CMD_PMS_ACTIVE, sizeof(CMD_PMS_ACTIVE));
    }

    // --- REPORTING TIMER (5 Seconds) ---
    // Calculates average, Updates Screen, Updates MQTT
    static unsigned long lastReport = 0;
    if (millis() - lastReport > 5000) {
        lastReport = millis();

        // 1. Calculate the Average
        int pm25Smooth = smooth(pm25Hist);
        int pm10Smooth = smooth(pm10Hist);

        // 2. Update Global Strings (Source of Truth)
        lastPM25 = String(pm25Smooth);
        lastPM10 = String(pm10Smooth);

        // 3. Update Screen
        if (lightsOn) drawDynamicValues();

        // 4. Update MQTT
        publishAll(); 
    }

    // --- SENSOR READING LOGIC ---
    // Runs as fast as possible to fill the buffer
    while(Serial1.available()) {
        byte b = Serial1.read();
        if(rxIndex < 128) rxBuffer[rxIndex++] = b;
        lastRxTime = millis();
    }
       
    if(rxIndex > 0 && millis() - lastRxTime > 50) {
        processSensorData();
        rxIndex = 0; 
    }
}

// Helper to generate the Select Dropdown
void printColorSelect(TCPClient &c, String name, uint16_t selected) {
    c.print("<select name='"); c.print(name); c.print("' style='width:100%;padding:10px;margin-bottom:10px;'>");
    for(int i=0; i<NUM_COLORS; i++) {
        c.print("<option value='"); c.print(colorValues[i]); c.print("'");
        if(colorValues[i] == selected) c.print(" selected");
        c.print(">"); c.print(colorNames[i]); c.print("</option>");
    }
    c.print("</select><br>");
}

void handleWebPortal() {
    TCPClient client = webServer.available();
    if (client) {
        char buf[1024]; int idx = 0; unsigned long t0 = millis();
        while (client.connected() && millis() - t0 < 1000) {   // the request can arrive after the connection
            if (!client.available()) { delay(1); continue; }
            char c = client.read();
            if (idx < 1023) buf[idx++] = c;
            if (c == '\n') break; 
        }
        buf[idx] = 0; String req = String(buf); 
        
        client.println("HTTP/1.1 200 OK"); client.println("Content-Type: text/html"); client.println("Connection: close"); client.println();
        
        if (req.indexOf("GET /fan?") != -1) {
            int i = req.indexOf("fs="); if (i != -1) fanSetStep(req.substring(i + 3).toInt());
            client.print("<html><head><meta http-equiv='refresh' content='0;url=/'></head><body>Fan step applied.</body></html>");
            client.stop(); return;
        }
        if (req.indexOf("GET /leds?") != -1) {
            auto arg = [&](String k) -> String {
                int s = req.indexOf("?" + k + "="); if (s == -1) s = req.indexOf("&" + k + "="); if (s == -1) return "";
                s += k.length() + 2; int e = req.indexOf("&", s), sp = req.indexOf(" ", s); if (e == -1 || e > sp) e = sp;
                return req.substring(s, e);
            };
            const char* bn[] = {"power", "a", "minus", "plus"};
            for (int c = 0; c < 2; c++) {
                String p = c ? "b" : "r";
                ledSet(c, "state", arg(p + "o")); ledSet(c, "follow", arg(p + "w"));
                if (arg(p + "b").length()) ledSet(c, "brightness", arg(p + "b"));
                if (arg(p + "f").length()) ledSet(c, "effect", arg(p + "f"));
                if (arg(p + "s").length()) ledSet(c, "speed", arg(p + "s"));
            }
            for (int i = 0; i < 4; i++) ledSet(1, bn[i], arg(String("k") + String(i)));
            ledsChanged();
            client.print("<html><head><meta http-equiv='refresh' content='0;url=/'></head><body>Lights applied.</body></html>");
            client.stop(); return;
        }
        if (req.indexOf("GET /save") != -1) {
            auto getVal = [&](String key) -> String {
                int s = req.indexOf(key + "="); if (s == -1) return ""; s += key.length() + 1;
                int e = req.indexOf("&", s); if (e == -1) e = req.indexOf(" ", s);
                String v = req.substring(s, e); 
                v.replace("+", " "); v.replace("%20", " "); 
                v.replace("%2F", "/"); v.replace("%2f", "/"); 
                return v;
            };
            
            String mi = getVal("mi"); String mu = getVal("mu"); String mp = getVal("mp"); String mt = getVal("mt");
            String mpo = getVal("mpo");
            String rst = getVal("reset");
            String dfu = getVal("dfu"); // <--- NEW: Get DFU param
            
            // Display & Colors
            String ffs = getVal("ffs"); String fx = getVal("fx"); String fy = getVal("fy");
            String pfs = getVal("pfs"); String px = getVal("px"); String py = getVal("py");
            String fc  = getVal("fc");  String pmlc = getVal("pmlc"); String pmvc = getVal("pmvc");
            
            // --- NEW: DFU LOGIC ---
            if (dfu == "1") {
                client.println("<h1>Entering DFU Mode...</h1>"); 
                client.println("<p>Device status LED will flash YELLOW. Connect USB to flash.</p>");
                client.stop(); 
                delay(1000); 
                System.dfu(false); // Enter DFU mode
                return;
            }
            // ----------------------

            if (rst == "1") {
                // To force reset, we empty the signature. Next boot it won't match.
                mySettings.build_signature[0] = 0; 
                EEPROM.put(EEPROM_ADDR, mySettings);
                client.println("<h1>Resetting...</h1>"); client.stop(); delay(1000); System.reset();
                return;
            }

            if(mi.length()>0) mi.toCharArray(mySettings.mqtt_ip, 32);
            if(mpo.length()>0) mySettings.mqtt_port = mpo.toInt();
            if(mu.length()>0) mu.toCharArray(mySettings.mqtt_user, 32);
            if(mp.length()>0) mp.toCharArray(mySettings.mqtt_pass, 32);
            if(mt.length()>0) mt.toCharArray(mySettings.topic_prefix, 64);
            
            if(ffs.length()>0) mySettings.fanFontSize = ffs.toInt();
            if(fx.length()>0)  mySettings.fanX = fx.toInt();
            if(fy.length()>0)  mySettings.fanY = fy.toInt();
            if(pfs.length()>0) mySettings.pmFontSize = pfs.toInt();
            if(px.length()>0)  mySettings.pmX = px.toInt();
            if(py.length()>0)  mySettings.pmY = py.toInt();

            // Save Colors
            if(fc.length()>0)   mySettings.fanColor = fc.toInt();
            if(pmlc.length()>0) mySettings.pmLabelColor = pmlc.toInt();
            if(pmvc.length()>0) mySettings.pmValueColor = pmvc.toInt();
            
            EEPROM.put(EEPROM_ADDR, mySettings);
            client.println("<h1>Saved! Rebooting...</h1>"); client.stop(); delay(1000); System.reset();
        } 
        else {
            client.print("<html><body style='font-family:sans-serif;padding:20px;'>");
            client.print("<h2>Configuration</h2><form action='/save' method='GET'>");
            
            client.print("<h3>MQTT</h3>");
            client.print("<b>IP:</b><br><input name='mi' value='"); client.print(mySettings.mqtt_ip); client.print("'><br>");
            client.print("<b>Port:</b><br><input name='mpo' value='"); client.print(mySettings.mqtt_port); client.print("'><br>");
            client.print("<b>User:</b><br><input name='mu' value='"); client.print(mySettings.mqtt_user); client.print("'><br>");
            client.print("<b>Pass:</b><br><input name='mp' value='"); client.print(mySettings.mqtt_pass); client.print("'><br>");
            client.print("<b>Topic:</b><br><input name='mt' value='"); client.print(mySettings.topic_prefix); client.print("'><br>");
            
            client.print("<h3>Display Settings</h3>");
            client.print("<b>Fan Font Size:</b><br><input name='ffs' value='"); client.print(mySettings.fanFontSize); client.print("'><br>");
            client.print("<b>Fan X:</b><br><input name='fx' value='"); client.print(mySettings.fanX); client.print("'><br>");
            client.print("<b>Fan Y:</b><br><input name='fy' value='"); client.print(mySettings.fanY); client.print("'><br>");
            client.print("<b>Fan Percentage Color:</b><br>"); printColorSelect(client, "fc", mySettings.fanColor);

            client.print("<hr>");
            client.print("<b>AQ Font Size:</b><br><input name='pfs' value='"); client.print(mySettings.pmFontSize); client.print("'><br>");
            client.print("<b>AQ X:</b><br><input name='px' value='"); client.print(mySettings.pmX); client.print("'><br>");
            client.print("<b>AQ Y:</b><br><input name='py' value='"); client.print(mySettings.pmY); client.print("'><br>");
            client.print("<b>AQ Value Color:</b><br>"); printColorSelect(client, "pmvc", mySettings.pmValueColor);
            client.print("<b>AQ Label Color:</b><br>"); printColorSelect(client, "pmlc", mySettings.pmLabelColor);
            
            client.print("<br><input type='submit' value='SAVE' style='padding:10px;width:100%;background:blue;color:white;'></form>");
            
            client.print(String("<hr><h3>Fan</h3><form action='/fan' method='GET'>Speed step <input type='number' name='fs' min='1' max='25' step='1' value='") + String(fp.step)
                + "' style='width:4em'>% (buttons, MQTT, web) <input type='submit' value='APPLY'></form>");
            client.print("<hr><h3>Lights</h3><form action='/leds' method='GET'>");
            for (int c = 0; c < 2; c++) {
                String p = c ? "b" : "r";
                client.print(String(c ? "<b>Buttons:</b> " : "<b>Ring:</b> ") + "<label><input type='checkbox' name='" + p + "o' value='1'" + (lp.on[c] ? " checked" : "") + "> on</label> ");
                client.print(String("<label><input type='checkbox' name='") + p + "w' value='1'" + (lp.follow[c] ? " checked" : "") + "> follows display</label><br>");
                client.print(String("Brightness <input type='range' name='") + p + "b' min='1' max='255' value='" + String(lp.bright[c]) + "'> Effect <select name='" + p + "f'>");
                for (int i = 0; i < (c ? LED_FX_N : 5); i++) client.print(String("<option") + (lp.fx[c] == i ? " selected" : "") + ">" + LED_FX[i] + "</option>");
                client.print(String("</select> Speed <input type='number' name='") + p + "s' min='10' max='250' step='1' value='" + String(lp.speed[c]) + "' style='width:4em'>%<br>");
            }
            const char* bl[] = {"power", "A", "minus", "plus"};
            for (int i = 0; i < 4; i++) client.print(String("<label><input type='checkbox' name='k") + String(i) + "' value='1'" + ((lp.mask >> i) & 1 ? " checked" : "") + "> " + bl[i] + "</label> ");
            client.print("<br><input type='submit' value='APPLY LIGHTS' style='padding:10px;width:100%;background:green;color:white;'></form>");

            // --- NEW: DFU BUTTON ---
            client.print("<hr><form action='/save' method='GET'><input type='hidden' name='dfu' value='1'>");
            client.print("<input type='submit' value='ENTER DFU MODE' style='background:orange;color:white;padding:10px;width:100%;'></form>");
            // -----------------------

            client.print("<hr><form action='/save' method='GET'><input type='hidden' name='reset' value='1'>");
            client.print("<input type='submit' value='FACTORY RESET' style='background:red;color:white;padding:10px;width:100%;'></form>");
            client.print("</body></html>");
        }
        client.stop();
    }
}

void http_handler(const char* url, ResponseCallback* cb, void* cbArg, Reader* body, Writer* result, void* reserved) {
    String req = String(url);
    if (req.startsWith("/save")) {
        auto getVal = [&](String key) -> String {
            int s = req.indexOf(key + "="); if (s == -1) return ""; s += key.length() + 1;
            int e = req.indexOf("&", s); if (e == -1) e = req.length();
            String v = req.substring(s, e); v.replace("+", " "); v.replace("%20", " "); return v;
        };
        String s = getVal("s"); String p = getVal("p");
        if (s.length() > 0) {
            s.toCharArray(mySettings.wifi_ssid, 64); p.toCharArray(mySettings.wifi_pass, 64);
            if(p.length()>0) WiFi.setCredentials(mySettings.wifi_ssid, mySettings.wifi_pass);
            else WiFi.setCredentials(mySettings.wifi_ssid);
            
            // SAVE SIGNATURE ON SOFTAP SETUP
            strncpy(mySettings.build_signature, CURRENT_BUILD_SIG, 24);
            EEPROM.put(EEPROM_ADDR, mySettings);
            
            cb(cbArg, 0, 200, "text/html", nullptr); result->write("<h1>Saved! Rebooting...</h1>"); delay(1000); System.reset();
        }
    } else {
        cb(cbArg, 0, 200, "text/html", nullptr);
        result->write("<html><body style='font-family:sans-serif;text-align:center;padding:20px;'><h2>Aeris WiFi Setup</h2><form action='/save' method='GET'>");
        result->write("<input name='s' placeholder='WiFi Name' required><br><input name='p' placeholder='WiFi Password' type='password'><br>");
        result->write("<button type='submit' style='padding:15px;width:100%;background:blue;color:white;'>CONNECT</button></form></body></html>");
    }
}

void drawDynamicValues() {
    if (!lightsOn) return; 
    
    // Use settings from struct
    int fSize = mySettings.fanFontSize;
    int fX = mySettings.fanX; int fY = mySettings.fanY;
    int pSize = mySettings.pmFontSize;
    int pX = mySettings.pmX; int pY = mySettings.pmY;

    if (currentSpeed != drawnSpeed) {
        drawnSpeed = currentSpeed;
        tft.setCursor(fX, fY); tft.setTextColor(mySettings.fanColor, ILI9341_BLACK); tft.setTextSize(fSize); 
        if (currentSpeed == 0) { 
            tft.print("OFF"); 
            tft.fillRect(fX + (3*6*fSize), fY, 120, 8*fSize, ILI9341_BLACK); 
        }
        else { 
            String s=String(currentSpeed); while(s.length()<3) s=" "+s; 
            tft.print(s); 
            tft.setTextSize(fSize / 2); // Make the % symbol smaller
            tft.print("% "); 
        }
    }
    if (!lastPM25.equals(drawnPM25)) {
        drawnPM25 = lastPM25;
        tft.setCursor(pX, pY); tft.setTextSize(pSize);
        
        // Print Value
        tft.setTextColor(mySettings.pmValueColor, ILI9341_BLACK);
        tft.print(lastPM25);
        
        // Print Label
        tft.setTextColor(mySettings.pmLabelColor, ILI9341_BLACK);
        tft.print(" ug/m3   "); 
    }
}

// Must run before tft.begin(): A4 becomes SPI MISO afterwards.
void initRing() {
    pinMode(PIN_RING_HS, OUTPUT); digitalWrite(PIN_RING_HS, HIGH); delay(5); digitalWrite(PIN_RING_HS, LOW); delay(5);
    digitalWrite(PIN_RING_HS, HIGH); pinMode(PIN_RING_HS, INPUT); delay(50);
    pinMode(PIN_RING_DATA, OUTPUT); pinMode(PIN_RING_CLK, OUTPUT); pinMode(PIN_RING_LATCH, OUTPUT);
    digitalWrite(PIN_RING_LATCH, HIGH); writeRing(0);
    // brightness 1-255 -> PWM 7-128 of 128 (gamma 2.2; 7 is the dimmest useful level, chosen by eye); lightness -> PWM
    for (int i = 0; i < 256; i++) { ledGamma[i] = i ? 7 + (int)(powf((i - 1) / 254.0f, 2.2f) * 121 + 0.5f) : 0; ledLum[i] = powf(i / 255.0f, 2.2f) * 128 + 0.5f; }
    EEPROM.get(LED_ADDR, lp);
    if (lp.magic != 0xA8 || lp.sum != ledSum() || lp.fx[0] > 4 || lp.fx[1] >= LED_FX_N || lp.speed[0] < 10 || lp.speed[1] < 10)
        lp = LedPrefs{0xA8, {0, 0}, {255, 255}, {0, 0}, {1, 1}, 15, {100, 100}, 0};
    for (int c = 0; c < 2; c++) if (lp.follow[c]) lp.on[c] = lightsOn;   // match the display, also in setup mode
    ledsChanged(); ledSave = false;
}

void writeRing(uint8_t v) {
    digitalWrite(PIN_RING_LATCH, LOW); shiftOut(PIN_RING_DATA, PIN_RING_CLK, LSBFIRST, v); digitalWrite(PIN_RING_LATCH, HIGH);
}

uint8_t ledSum() { uint8_t s = 0x5A; for (unsigned i = 0; i < sizeof(lp) - 1; i++) s += ((uint8_t*)&lp)[i]; return s; }

// Effect lightness 0..1 -> PWM: brief zero, then from just-visible (lightness 46 = 3/128) up to the channel level.
uint8_t ledAt(float l, int c) {
    if (l <= 0) return 0; if (l >= 1) return ledGamma[lp.bright[c]];
    uint8_t hi = ledHi[c], lo = hi < 46 ? hi : 46; return ledLum[(uint8_t)(lo + (hi - lo) * l)];
}

// Lightness 0..1 of light e of n at time t (ms). Multi-light effects (5+) on a single light are solid.
float ledFx(uint8_t fx, int e, int n, uint32_t t, float cand, float tw, uint8_t sp) {
    if (fx >= 5 && n < 2) return 1;
    float x;
    switch (fx) {
        case 1: return (t / 500) % 2 ? 0 : 1;
        case 2: case 8: if (fx == 8) t += e * 750; x = (t % 3000) / 1500.0f; if (x > 1) x = 2 - x; return x < 0.05f ? 0 : (x - 0.05f) / 0.95f;
        case 3: {   // two soft swells, then a pause capped at ~3 s of real time (21 virtual ms per speed %)
            uint32_t q = 21u * sp; if (q > 1600) q = 1600; t %= 800 + q;
            return t < 300 ? sinf(t * 3.1416f / 300) : t >= 500 && t < 800 ? 0.7f * sinf((t - 500) * 3.1416f / 300) : 0;
        }
        case 4: return cand;
        case 5: return (int)((t / 250) % n) == e ? 1 : 0;
        case 6: { int k = (t / 200) % (2 * n - 2); return (k < n ? k : 2 * n - 2 - k) == e ? 1 : 0; }
        case 7: return (int)((t / 300) % (n + 1)) > e ? 1 : 0;
        case 9: return tw;
    }
    return 1;
}

// 1 ms tick: effect lightness per light -> PWM duty for the interrupt (ring = register bits 6 + boost 0, buttons = 2-5).
void ledsTick() {
    static float cand[5] = {1, 1, 1, 1, 1}, ct[5] = {1, 1, 1, 1, 1}; static int16_t tw[4]; static uint32_t vt[2]; static uint16_t acc[2];
    uint8_t d[8] = {0};
    for (int c = 0; c < 2; c++) {   // effect time per channel runs at speed% x 0.7 of real time
        acc[c] += lp.speed[c] * 7;
        while (acc[c] >= 1000) {
            acc[c] -= 1000; vt[c]++;
            for (int e = c ? 0 : 4; e < (c ? 4 : 5); e++) { if (random(60) == 0) ct[e] = random(55, 101) / 100.0f; cand[e] += (ct[e] - cand[e]) * 0.05f; }
            if (c) for (int e = 0; e < 4; e++) { if (tw[e]) { if (++tw[e] > 1200) tw[e] = 0; } else if (random(1500) == 0) tw[e] = 1; }
        }
    }
    if (lp.on[0]) d[6] = d[0] = ledAt(ledFx(lp.fx[0], 0, 1, vt[0], cand[4], 0, lp.speed[0]), 0);
    if (lp.on[1]) for (int e = 0; e < 4; e++) if (lp.mask & (1 << e))
        d[2 + e] = ledAt(ledFx(lp.fx[1], e, 4, vt[1], cand[e], tw[e] < 600 ? tw[e] / 600.0f : (1200 - tw[e]) / 600.0f, lp.speed[1]), 1);
    for (int i = 0; i < 8; i++) ledDuty[i] = d[i];
}

// 20 kHz hardware timer (TIM6, no pins): 128-slot PWM per register bit at 156 Hz, register rewritten only on change.
void ledsPwm() {
    static uint8_t slot = 0, last = 0xFF; uint8_t out = 0;
    for (int i = 0; i < 8; i++) if (slot < ledDuty[i]) out |= 1 << i;
    slot = (slot + 1) & 127;
    if (out != last) { last = out; writeRing(out); }
}

bool ledTrue(String v) { v.toLowerCase(); return v == "on" || v == "true" || v == "1"; }

void ledSet(int c, String k, String v) {
    const char* bn[] = {"power", "a", "minus", "plus"};
    if (k == "state") lp.on[c] = ledTrue(v);
    else if (k == "brightness") lp.bright[c] = constrain(v.toInt(), 0, 255);
    else if (k == "effect") { for (int i = 0; i < (c ? LED_FX_N : 5); i++) if (v == LED_FX[i]) lp.fx[c] = i; }
    else if (k == "follow") lp.follow[c] = ledTrue(v);
    else if (k == "speed") lp.speed[c] = constrain(v.toInt(), 10, 250);
    else if (c) { for (int i = 0; i < 4; i++) if (k == bn[i]) lp.mask = ledTrue(v) ? lp.mask | (1 << i) : lp.mask & ~(1 << i); }
}

// After changes: recompute the brightness range, save in 3 s (EEPROM wear), publish the retained state.
void ledsChanged() {
    for (int c = 0; c < 2; c++) ledHi[c] = 255 * powf(ledGamma[lp.bright[c]] / 128.0f, 1 / 2.2f) + 0.5f;
    ledDirty = millis(); ledSave = true; ledPublish = true;
}

void publishLeds() {
    const char* tf[] = {"false", "true"}; const char* bn[] = {"power", "a", "minus", "plus"};
    String p = String(mySettings.topic_prefix);
    for (int c = 0; c < 2; c++) {
        String s = String("{\"state\":\"") + (lp.on[c] ? "ON" : "OFF") + "\",\"brightness\":" + String(lp.bright[c]) + ",\"effect\":\"" + LED_FX[lp.fx[c]]
            + "\",\"color_mode\":\"brightness\",\"follow\":" + tf[lp.follow[c] ? 1 : 0] + ",\"speed\":" + String(lp.speed[c]);
        if (c) for (int i = 0; i < 4; i++) s += String(",\"") + bn[i] + "\":" + tf[(lp.mask >> i) & 1];
        client->publish((p + (c ? "buttons/state" : "ring/state")).c_str(), (s + "}").c_str(), true);
    }
}

void updateSystem() {
    static int lastPwm = 0; int pwm = FAN_PWM[constrain(currentSpeed, 0, 100)];
    if (currentSpeed != fp.speed || savedSpeed != fp.saved) { fanSave = true; fanDirtyAt = millis(); }
    if (pwm && !lastPwm) { fanKick = true; fanKickAt = millis(); } if (!pwm) fanKick = false; lastPwm = pwm;
    analogWrite(PIN_FAN, fanKick ? max(pwm, 64) : pwm);
    int s = lightsOn ? HIGH : LOW;
    digitalWrite(PIN_DISP_BL, s);
    static bool lastLights = false;
    if (lightsOn != lastLights) { lastLights = lightsOn; for (int c = 0; c < 2; c++) if (lp.follow[c]) lp.on[c] = lightsOn; ledPublish = true; }
    if(lightsOn) { if (drawnSpeed == -999) { tft.fillScreen(ILI9341_BLACK); drawnSpeed=-1; drawnPM25="-1"; } drawDynamicValues(); }
    else { drawnSpeed = -999; }
    if (!fanHeld) publishAll();
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    String t = String(topic);
    String pre = String(mySettings.topic_prefix);
    if (t == pre + "ring/set" || t == pre + "buttons/set") {
        int c = t == pre + "buttons/set"; JSONValue o = JSONValue::parseCopy(message.c_str(), message.length());
        if (!o.isObject()) ledSet(c, "state", message);
        else { JSONObjectIterator it(o); while (it.next()) { JSONValue v = it.value();
            ledSet(c, String(it.name().data()), v.isBool() ? String(v.toBool() ? "true" : "false") : v.isNumber() ? String(v.toInt()) : String(v.toString().data())); } }
        ledsChanged(); return;
    }
    if (t == pre + "fan/step/set") { fanSetStep(message.toInt()); return; }
    if (t.endsWith("/set")) {
        if (t.indexOf("lights") > -1) { lightsOn = (message.toInt() == 1); }
        else {
            int s = message.toInt(); currentSpeed = fanSnap(map(s, 0, 255, 0, 100)); if (s > 0 && currentSpeed == 0) currentSpeed = fp.step;
            if(currentSpeed > 100) currentSpeed=100;
            if(currentSpeed > 0) { savedSpeed = currentSpeed; lightsOn = true; } else lightsOn = false;
        }
        updateSystem();
    }
}

void checkButton(int pin, int idx) {
    int reading = digitalRead(pin);
    if (reading != lastBtnState[idx]) {
        delay(50); 
        if (digitalRead(pin) == reading) {
            lastBtnState[idx] = reading;
            if (reading == LOW) { btnRepAt[idx] = millis(); btnReps[idx] = 0; handlePress(pin); }
            else if (fanHeld && idx < 2) { fanHeld = false; publishAll(); }
        }
    }
    else if (reading == LOW && idx < 2) {   // + / - held: repeat after 0.4 s, then faster (about 20 % per second)
        unsigned long fast = 50UL * fp.step, iv = btnReps[idx] == 0 ? 400 : btnReps[idx] < 5 ? (fast > 150 ? fast : 150) : fast;
        if (millis() - btnRepAt[idx] >= iv) { btnRepAt[idx] = millis(); btnReps[idx]++; fanHeld = true; handlePress(pin); }
    }
}

int fanSnap(int pct) { int s = fp.step; pct = s * ((pct + s / 2) / s); return constrain(pct, 0, 100); }

uint8_t fanSum() { return fp.magic ^ fp.step ^ fp.speed ^ fp.saved ^ 0x5A; }

void fanLoad() {
    EEPROM.get(FAN_ADDR, fp);
    if (fp.magic != 0xF6 || fp.sum != fanSum() || fp.step < 1 || fp.step > 25 || fp.speed > 100 || fp.saved > 100) { fp = FanPrefs{0xF6, 1, 25, 25, 0}; fp.sum = fanSum(); }
}

void fanSetStep(int s) {
    fp.step = constrain(s, 1, 25); fp.sum = fanSum(); EEPROM.put(FAN_ADDR, fp); fanStepPublish = true;
    if (currentSpeed > 0) { int v = fanSnap(currentSpeed); currentSpeed = v < fp.step ? fp.step : v; savedSpeed = currentSpeed; }
    updateSystem();
}

void handlePress(int pin) {
    if (pin == BTN_POWER) {
        if (currentSpeed > 0) { savedSpeed = currentSpeed; currentSpeed = 0; lightsOn = false; }
        else { if(savedSpeed < 1) savedSpeed = 25; currentSpeed = savedSpeed; lightsOn = true; }
    }
    else if (pin == BTN_UP) { if(currentSpeed==0){currentSpeed=fp.step;lightsOn=true;}else currentSpeed+=fp.step; if(currentSpeed>100)currentSpeed=100; savedSpeed=currentSpeed; }
    else if (pin == BTN_DOWN) { if(currentSpeed>0){currentSpeed-=fp.step;if(currentSpeed<=0)currentSpeed=0;else savedSpeed=currentSpeed;} }   // at 0 the screen stays on, showing OFF
    else if (pin == BTN_EXTRA) { lightsOn = !lightsOn; }
    updateSystem();
}

void sendRawByte(const byte* data, int len) {
    for (int i = 0; i < len; i++) {
        byte b = data[i];
        digitalWrite(PIN_SENSOR_TX, LOW); delayMicroseconds(104); 
        for (int j = 0; j < 8; j++) { digitalWrite(PIN_SENSOR_TX, (b & 1) ? HIGH : LOW); delayMicroseconds(104); b >>= 1; }
        digitalWrite(PIN_SENSOR_TX, HIGH); delayMicroseconds(104);
    }
}

int smooth(int *arr) {
    long sum = 0;
    int count = histFilled ? WINDOW : histIndex;
    
    // SAFETY CHECK: Prevent Divide by Zero crash
    if (count == 0) return 0; 
    
    for (int i = 0; i < count; i++) sum += arr[i];
    return sum / count;
}

void processSensorData() {
    int startIdx = -1;

    // 1. Search for Custom Header 0x32 0x3D
    for (int i = 0; i < rxIndex - 1; i++) {
        if (rxBuffer[i] == 0x32 && rxBuffer[i+1] == 0x3D) {
            startIdx = i;
            break;
        }
    }

    if (startIdx != -1 && startIdx + 32 <= rxIndex) {
        
        // 2. VERIFY CHECKSUM
        int calcChecksum = 0;
        for (int i = 0; i < 30; i++) {
            calcChecksum += rxBuffer[startIdx + i];
        }
        int sentChecksum = (rxBuffer[startIdx + 30] << 8) + rxBuffer[startIdx + 31];

        if (calcChecksum == sentChecksum) {            
            // 3. READ ATMOSPHERIC DATA (Indices 12 & 14)
            int pm25Val = (rxBuffer[startIdx + 12] << 8) + rxBuffer[startIdx + 13];
            int pm10Val = (rxBuffer[startIdx + 14] << 8) + rxBuffer[startIdx + 15];

            pm25Hist[histIndex] = pm25Val;
            pm10Hist[histIndex] = pm10Val;
            histIndex++;
            if (histIndex >= WINDOW) {
                histIndex = 0;
                histFilled = true;
            }

            // Reset index so we don't re-read the same packet
            rxIndex = 0; 
        }
    }
}

void publishAll() {
    if (client != NULL && client->isConnected()) {
        String p = String(mySettings.topic_prefix);
        int pwm = map(currentSpeed, 0, 100, 0, 255);
        client->publish(p + "fan/state", String(pwm)); delay(20);
        client->publish(p + "fan/setpoint", String(pwm)); delay(20);
        client->publish(p + "sensor/pm25", lastPM25); delay(20);
        client->publish(p + "sensor/pm10", lastPM10);
    }

}
