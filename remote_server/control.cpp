#include <Wifi.h>
#include <WebServer.h>
#include <LittleFS.h> 


const char *ssid = "C3-P-Own"; //ap name
const char *password = "pl$Ch@ng3Me"; //password

WebServer server(80); //open controller web page on 192.168.4.1:80

//pin configuration (for esp32-c3-super-mini)
#define RX_PIN 20
#define TX_PIN 21

String victimOS = "Detecting...\n";
String exfilLog = "Awaiting execution...\n";

//file Handler
void handleRoot() {
    if(LittleFS.exists("/index.html")){
        File file = LittleFS.open("/index.html", "r");
        server.streamFile(file, "text/html");
        file.close();
    } 
    
    else{
        server.send(404, "text/plain", "Internal Error: index.html missing from Flash memory.");
    }
}

void handleExecute() {
  if (server.hasArg("val")) {
    Serial1.println(server.arg("val"));
    server.send(200, "text/plain", "OK");
  }
}

void handleStatus(){
  String json = "{\"os\":\"" + currentOS + "\",\"logs\":\"" + exfilLog + "\"}";
  server.send(200, "application/json", json);
}

void setup(){
    Serial1.begin(115200, SERIAL_8N1, RX_PIN, TX_PIN);
  
    // Initialize filesystem storage partition
    if(!LittleFS.begin(true)){
        // True tells it to format the storage partition if it's corrupt or empty
        Serial.println("LittleFS Mount Failed");
    }
}

void handleRoot(){
    server.send(200, "text/html", html_page); 
}

void handleExecute(){
  if(server.hasArg("val")){
    Serial1.println(server.arg("val"));
    server.send(200, "text/plain", "OK");
  }
}

void handleStatus(){
  String json = "{\"os\":\"" + currentOS + "\",\"logs\":\"" + exfilLog + "\"}";
  server.send(200, "application/json", json);
}

