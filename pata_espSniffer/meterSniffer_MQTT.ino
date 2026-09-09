#include <Arduino.h>
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <PubSubClient.h>

// --- 1. WIFI KONFIGURACE ---
const char* ssid = "Technical support only";
const char* password = "goodwe2010";

IPAddress local_IP(192, 168, 68, 135);
IPAddress gateway(192, 168, 68, 1);
IPAddress subnet(255, 255, 255, 0);
IPAddress primaryDNS(8, 8, 8, 8);

// --- 2. HIVEMQ CLOUD KONFIGURACE ---
// Zadejte pouze čistou adresu (BEZ wss:// a BEZ portu)
const char* mqtt_server = "109e3418cc4e472ca8be85f97368e4da.s1.eu.hivemq.cloud"; 
const int   mqtt_port   = 8883;                 // Šifrovaný port
const char* mqtt_user   = "BayWa_pata";            // Uživatelské jméno z Access Control
const char* mqtt_pass   = "Distribuce2016";     // Heslo k tomuto účtu
const char* mqtt_topic  = "baywa/mereni/hlavni";// Téma (pro garáž změníte na baywa/mereni/garaz)

// --- 3. HARDWARE PINY PRO RS485 ---
#define RX_PIN 5  
#define TX_PIN 4  

WiFiClientSecure espClient;
PubSubClient client(espClient);

uint8_t buffer[256];
size_t bufIndex = 0;
unsigned long lastRxTime = 0;

uint16_t calculateCRC(const uint8_t *data, uint8_t length) {
  uint16_t crc = 0xFFFF;
  for (uint8_t i = 0; i < length; i++) {
    crc ^= data[i];
    for (uint8_t j = 0; j < 8; j++) {
      if (crc & 0x0001) crc = (crc >> 1) ^ 0xA001;
      else crc >>= 1;
    }
  }
  return crc;
}

void connectMQTT() {
  while (!client.connected()) {
    Serial.print("Připojování k HiveMQ Cloud...");
    String clientId = "ESP32_GM3000_" + String(random(0xffff), HEX);
    if (client.connect(clientId.c_str(), mqtt_user, mqtt_pass)) {
      Serial.println(" Připojeno!");
    } else {
      Serial.print(" Chyba, rc=");
      Serial.print(client.state());
      Serial.println(" Opakuji za 3 sekundy...");
      delay(3000);
    }
  }
}

void parseGM3000Response(const uint8_t *frame) {
  // CRC kontrola
  uint16_t expectedCRC = frame[79] | (frame[80] << 8);
  if (calculateCRC(frame, 79) != expectedCRC) return;

  // Dekódování napětí
  float v1 = ((frame[3] << 8) | frame[4]) / 10.0f;
  float v2 = ((frame[5] << 8) | frame[6]) / 10.0f;
  float v3 = ((frame[7] << 8) | frame[8]) / 10.0f;

  // Dekódování proudů
  int32_t i1_raw = ((uint32_t)frame[9] << 24) | ((uint32_t)frame[10] << 16) | ((uint32_t)frame[11] << 8) | frame[12];
  int32_t i2_raw = ((uint32_t)frame[13] << 24) | ((uint32_t)frame[14] << 16) | ((uint32_t)frame[15] << 8) | frame[16];
  int32_t i3_raw = ((uint32_t)frame[17] << 24) | ((uint32_t)frame[18] << 16) | ((uint32_t)frame[19] << 8) | frame[20];

  // Dekódování výkonů
  int32_t p1 = ((uint32_t)frame[21] << 24) | ((uint32_t)frame[22] << 16) | ((uint32_t)frame[23] << 8) | frame[24];
  int32_t p2 = ((uint32_t)frame[25] << 24) | ((uint32_t)frame[26] << 16) | ((uint32_t)frame[27] << 8) | frame[28];
  int32_t p3 = ((uint32_t)frame[29] << 24) | ((uint32_t)frame[30] << 16) | ((uint32_t)frame[31] << 8) | frame[32];

  // Sestavení JSON řetězce
  char jsonBuffer[220];
  snprintf(jsonBuffer, sizeof(jsonBuffer),
    "{\"p1\":%d,\"p2\":%d,\"p3\":%d,\"v1\":%.0f,\"v2\":%.0f,\"v3\":%.0f,\"i1\":%.1f,\"i2\":%.1f,\"i3\":%.1f}",
    p1, p2, p3, v1, v2, v3, i1_raw / 100.0f, i2_raw / 100.0f, i3_raw / 100.0f);

  // Odeslání dat do HiveMQ Cloudu
  if (client.connected()) {
    client.publish(mqtt_topic, jsonBuffer);
    Serial.print("MQTT Odesláno: ");
    Serial.println(jsonBuffer);
  }
}

void setup() {
  Serial.begin(115200);
  Serial1.begin(9600, SERIAL_8N1, RX_PIN, TX_PIN);

  if (!WiFi.config(local_IP, gateway, subnet, primaryDNS)) {
    Serial.println("Chyba při nastavení IP!");
  }

  WiFi.begin(ssid, password);
  while (WiFi.status() != WL_CONNECTED) { 
    delay(200); 
  }
  
  WiFi.setSleep(false);
  Serial.println("\nPřipojeno k Wi-Fi bez spánkového režimu!");

  // Nastavení MQTTS šifrování
  espClient.setInsecure(); // Přeskočení ověřování CA certifikátu
  client.setServer(mqtt_server, mqtt_port);
  client.setBufferSize(512);

  connectMQTT();
}

void loop() {
  // Kontrola Wi-Fi a MQTT připojení
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.begin(ssid, password);
    while (WiFi.status() != WL_CONNECTED) { delay(200); }
  }
  if (!client.connected()) {
    connectMQTT();
  }
  client.loop();

  // 1. Plnění vyrovnávací paměti z RS485
  while (Serial1.available()) {
    uint8_t b = Serial1.read();
    if (bufIndex < sizeof(buffer)) {
      buffer[bufIndex++] = b;
    }
    lastRxTime = millis();
  }

  // 2. Detekce konce Modbus rámečku
  if (bufIndex > 0 && (millis() - lastRxTime > 10)) {
    if (bufIndex >= 81) {
      for (size_t i = 0; i <= bufIndex - 81; i++) {
        if (buffer[i] == 0x03 && buffer[i + 1] == 0x03 && buffer[i + 2] == 0x4C) {
          parseGM3000Response(&buffer[i]);
          break;
        }
      }
    }
    bufIndex = 0;
  }
}