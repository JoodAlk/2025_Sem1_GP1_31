#include <WiFi.h>
#include <Firebase_ESP_Client.h>
#include "addons/TokenHelper.h"
#include "addons/RTDBHelper.h"

// ===== Wi-Fi =====
#define WIFI_SSID     "HUAWEI-B525-7260"
#define WIFI_PASSWORD "58658593"

// ===== Firebase (RTDB) =====
#define API_KEY      "AIzaSyBgzhlooyfDirhNsYww63URZfMhhl2DDhE"
#define DATABASE_URL "https://baseer-40cf2-default-rtdb.asia-southeast1.firebasedatabase.app/"

// ===== Project config =====
#define BIN_ID         "BIN-001"
#define SENSOR_ID      "esp-91AC"
#define SEND_INTERVAL  3000UL   // ms between uploads

// ===== Ultrasonic (ESP32) =====
#define TRIG_PIN 12
#define ECHO_PIN 13

// ===== Retry / Backoff settings =====
#define WIFI_RETRY_MAX          8
#define WIFI_RETRY_DELAY_MS     500
#define FB_RETRY_MAX            3
#define FB_BASE_BACKOFF_MS      400   // 400, 800, 1600

FirebaseData fbdo;
FirebaseAuth auth;
FirebaseConfig config;

unsigned long lastSend = 0;
bool signupOK = false;

// offline buffer for last payload if upload failed
bool hasBufferedPayload = false;
FirebaseJson bufferedJson;

// --- utils ---
bool ensureWiFi() {
  if (WiFi.status() == WL_CONNECTED) return true;

  WiFi.disconnect(true);
  delay(100);
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);

  for (int i = 0; i < WIFI_RETRY_MAX; i++) {
    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("Wi-Fi reconnected. IP: " + WiFi.localIP().toString());
      return true;
    }
    Serial.print(".");
    delay(WIFI_RETRY_DELAY_MS);
  }
  Serial.println("\nWi-Fi not available.");
  return false;
}

bool ensureFirebaseSession() {
  if (!signupOK) {
    if (Firebase.signUp(&config, &auth, "", "")) {
      signupOK = true;
      Serial.println("Firebase sign-in OK");
    } else {
      Serial.printf("Sign-in error: %s\n", config.signer.signupError.message.c_str());
      return false;
    }
  }
  return true;
}

// *** FIX: accept non-const reference so we can pass FirebaseJson* to updateNode ***
bool sendToFirebase(FirebaseJson &j) {
  String path = String("/bins/") + BIN_ID;

  for (int attempt = 0; attempt < FB_RETRY_MAX; attempt++) {
    if (!ensureWiFi()) return false;
    if (!ensureFirebaseSession()) { delay(FB_BASE_BACKOFF_MS * (1 << attempt)); continue; }

    if (Firebase.ready()) {
      // updateNode requires FirebaseJson* (non-const)
      if (Firebase.RTDB.updateNode(&fbdo, path.c_str(), &j)) {
        return true;
      } else {
        Serial.printf("Firebase attempt %d failed: %s\n",
                      attempt + 1, fbdo.errorReason().c_str());
      }
    } else {
      Serial.println("Firebase not ready; retrying...");
    }
    delay(FB_BASE_BACKOFF_MS * (1 << attempt)); // 400, 800, 1600
  }
  return false;
}

// --- read distance in cm (returns -1 if no echo) ---
int readDistanceCm() {
  digitalWrite(TRIG_PIN, LOW);
  delayMicroseconds(2);
  digitalWrite(TRIG_PIN, HIGH);
  delayMicroseconds(10);
  digitalWrite(TRIG_PIN, LOW);

  long duration = pulseIn(ECHO_PIN, HIGH, 30000);
  if (duration == 0) return -1;
  return (int)(duration * 0.034f / 2.0f);
}

void setup() {
  Serial.begin(115200);
  pinMode(TRIG_PIN, OUTPUT);
  pinMode(ECHO_PIN, INPUT);

  // ---- Wi-Fi ----
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  Serial.print("Connecting to Wi-Fi");
  for (int i = 0; i < WIFI_RETRY_MAX && WiFi.status() != WL_CONNECTED; i++) {
    Serial.print(".");
    delay(WIFI_RETRY_DELAY_MS);
  }
  if (WiFi.status() == WL_CONNECTED) {
    Serial.println("\nWi-Fi connected. IP: " + WiFi.localIP().toString());
  } else {
    Serial.println("\nStarting without Wi-Fi (will retry).");
  }

  // ---- Firebase ----
  config.api_key = API_KEY;
  config.database_url = DATABASE_URL;
  Firebase.begin(&config, &auth);
  Firebase.reconnectWiFi(true);

  if (Firebase.signUp(&config, &auth, "", "")) {
    signupOK = true;
    Serial.println("Firebase sign-in OK");
  } else {
    Serial.printf("Initial sign-in error: %s\n", config.signer.signupError.message.c_str());
  }
}

void loop() {
  const unsigned long now = millis();

  // Try to flush buffered payload if online again
  if (hasBufferedPayload && ensureWiFi() && ensureFirebaseSession() && Firebase.ready()) {
    if (sendToFirebase(bufferedJson)) {
      Serial.println("Buffered payload sent.");
      hasBufferedPayload = false;
      bufferedJson.clear();
    }
  }

  if (now - lastSend < SEND_INTERVAL) return;
  lastSend = now;

  int distance = readDistanceCm();
  if (distance < 0) {
    Serial.println("Sensor read failed (no echo).");
    return;
  }

  FirebaseJson j;
  j.set("DistanceCm", distance);
  j.set("lastUpdate", (int)now);
  j.set("Sensor/Id", SENSOR_ID);

  if (sendToFirebase(j)) {
    Serial.printf("Updated /bins/%s → Distance=%d cm\n", BIN_ID, distance);
  } else {
    // Buffer last payload
    bufferedJson = j;
    hasBufferedPayload = true;
    Serial.println("Upload failed; payload buffered (will retry).");
  }
}