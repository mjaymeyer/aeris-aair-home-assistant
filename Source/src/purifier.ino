#include <MQTT.h>
#include "Adafruit_ILI9341.h"
#include "Adafruit_mfGFX.h"
#include "softap_http.h"

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
const int PIN_RING_EN   = D6; 
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

int currentSpeed = 0;      
int savedSpeed   = 25;     
bool lightsOn    = true;   
String lastPM25 = "0";
String lastPM10 = "0";
int drawnSpeed = -999;     
String drawnPM25 = "-999"; 
int lastBtnState[] = {0,0,0,0}; 
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
void sendRawByte(const byte* data, int len); void processSensorData(); void publishAll();
void enterSetupMode(); void enterNormalMode(); void loadDefaults();
void printColorSelect(TCPClient &c, String name, uint16_t selected);

STARTUP(System.set(SYSTEM_CONFIG_SOFTAP_PREFIX, "Aeris"));
STARTUP(softap_set_application_page_handler(http_handler, nullptr));

void setup() {
    Serial.begin(9600);
    
    pinMode(PIN_FAN, OUTPUT);
    pinMode(PIN_SENSOR_TX, OUTPUT); digitalWrite(PIN_SENSOR_TX, HIGH); 
    pinMode(PIN_DISP_BL, OUTPUT); pinMode(PIN_RING_EN, OUTPUT);
    pinMode(BTN_UP, INPUT_PULLDOWN); pinMode(BTN_DOWN, INPUT_PULLDOWN);
    pinMode(BTN_EXTRA, INPUT_PULLDOWN); pinMode(BTN_POWER, INPUT_PULLDOWN);
    
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
    
    // Save the CURRENT binary's timestamp so we don't reset on next boot
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
    
    currentSpeed = 25; savedSpeed = 25; lightsOn = true;
    updateSystem();
    drawDynamicValues();
}

void loop() {
    if (WiFi.listening()) {
        return; 
    }

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
                client->connect("Aeris", mySettings.mqtt_user, mySettings.mqtt_pass);
                if (client->isConnected()) {
                    client->subscribe(String(mySettings.topic_prefix) + "fan/set");
                    client->subscribe(String(mySettings.topic_prefix) + "lights/set");
                    publishAll();
                    RGB.color(0, 255, 255); 
                }
            } else {
                client->loop();
            }
        }
    }

    checkButton(BTN_POWER, 3); checkButton(BTN_UP, 0); checkButton(BTN_DOWN, 1); checkButton(BTN_EXTRA, 2);

    static unsigned long lastHb = 0;
    if (millis() - lastHb > 5000) {
        lastHb = millis();
        sendRawByte(CMD_HPMA_START, sizeof(CMD_HPMA_START)); delay(50);
        sendRawByte(CMD_PMS_WAKE, sizeof(CMD_PMS_WAKE)); delay(50);
        sendRawByte(CMD_PMS_ACTIVE, sizeof(CMD_PMS_ACTIVE));
        publishAll();
    }

    while(Serial1.available()) {
            byte b = Serial1.read();
            if(rxIndex < 128) rxBuffer[rxIndex++] = b;
            lastRxTime = millis();
        }
        
        if(rxIndex > 0 && millis() - lastRxTime > 50) {
            
            // --- START DEBUG BLOCK ---
            Serial.print("RX Len: "); Serial.print(rxIndex); Serial.print(" Data: ");
            for(int i=0; i < rxIndex; i++) {
                // Print a leading zero if needed (e.g., "0F" instead of "F")
                if(rxBuffer[i] < 0x10) Serial.print("0");
                Serial.print(rxBuffer[i], HEX); 
                Serial.print(" ");
            }
            Serial.println(); 
            // --- END DEBUG BLOCK ---

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
        char buf[1024]; int idx = 0;
        while (client.connected() && client.available()) {
            char c = client.read();
            if (idx < 1023) buf[idx++] = c;
            if (c == '\n') break; 
        }
        buf[idx] = 0; String req = String(buf); 
        
        client.println("HTTP/1.1 200 OK"); client.println("Content-Type: text/html"); client.println("Connection: close"); client.println();
        
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
            
            // Display & Colors
            String ffs = getVal("ffs"); String fx = getVal("fx"); String fy = getVal("fy");
            String pfs = getVal("pfs"); String px = getVal("px"); String py = getVal("py");
            String fc  = getVal("fc");  String pmlc = getVal("pmlc"); String pmvc = getVal("pmvc");
            
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

void updateSystem() {
    analogWrite(PIN_FAN, map(currentSpeed, 0, 100, 0, 255));
    int s = lightsOn ? HIGH : LOW;
    digitalWrite(PIN_DISP_BL, s); digitalWrite(PIN_RING_EN, !s); 
    if(lightsOn) { if (drawnSpeed == -999) { tft.fillScreen(ILI9341_BLACK); drawnSpeed=-1; drawnPM25="-1"; } drawDynamicValues(); }
    else { drawnSpeed = -999; }
    publishAll();
}

void mqtt_callback(char* topic, byte* payload, unsigned int length) {
    String message = "";
    for (unsigned int i = 0; i < length; i++) message += (char)payload[i];
    String t = String(topic);
    if (t.endsWith("/set")) {
        if (t.indexOf("lights") > -1) { lightsOn = (message.toInt() == 1); }
        else {
            int s = message.toInt(); currentSpeed = map(s, 0, 255, 0, 100); currentSpeed = 5 * ((currentSpeed + 2)/5);
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
            if (reading == HIGH) handlePress(pin);
        }
    }
}

void handlePress(int pin) {
    if (pin == BTN_POWER) {
        if (currentSpeed > 0) { savedSpeed = currentSpeed; currentSpeed = 0; lightsOn = false; }
        else { if(savedSpeed < 10) savedSpeed = 10; currentSpeed = savedSpeed; lightsOn = true; }
    }
    else if (pin == BTN_UP) { if(currentSpeed==0){currentSpeed=5;lightsOn=true;}else currentSpeed+=5; if(currentSpeed>100)currentSpeed=100; savedSpeed=currentSpeed; }
    else if (pin == BTN_DOWN) { if(currentSpeed>0){currentSpeed-=5;if(currentSpeed<=0){currentSpeed=0;lightsOn=false;}else savedSpeed=currentSpeed;} }
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
        // Sum bytes 0 to 29
        for (int i = 0; i < 30; i++) {
            calcChecksum += rxBuffer[startIdx + i];
        }
        int sentChecksum = (rxBuffer[startIdx + 30] << 8) + rxBuffer[startIdx + 31];

        if (calcChecksum == sentChecksum) {
            // Checksum matches! Data is valid.
            
            // 3. READ STANDARD DATA
            // Bytes 6-7: PM2.5 Standard
            // Bytes 8-9: PM10 Standard
            int pm25Val = (rxBuffer[startIdx + 6] << 8) + rxBuffer[startIdx + 7];
            int pm10Val = (rxBuffer[startIdx + 8] << 8) + rxBuffer[startIdx + 9];

            bool ignoreSample = false;

            if (pm25Val == 0) {
                zeroCount++;
                if (zeroCount < 5) {
                    ignoreSample = true; // Ignore the first 4 zeros
                }
            } else {
                zeroCount = 0; // We saw a real number, reset counter
            }

            // Only update history if we aren't ignoring this sample
            if (!ignoreSample) {
                pm25Hist[histIndex] = pm25Val;
                pm10Hist[histIndex] = pm10Val;
                histIndex++;
                if (histIndex >= WINDOW) {
                    histIndex = 0;
                    histFilled = true;
                }
            }

            int pm25Smooth = smooth(pm25Hist);
            int pm10Smooth = smooth(pm10Hist);

            lastPM25 = String(pm25Smooth);
            lastPM10 = String(pm10Smooth);

            if (lightsOn) drawDynamicValues();
            publishAll();
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
