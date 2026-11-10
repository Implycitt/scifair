#include <Wire.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <esp_system.h>
#include <esp_sleep.h>
#include <esp_random.h>
#include <SensirionI2cScd4x.h>
#include "secrets.h"

#undef NO_ERROR
#define NO_ERROR 0

const char* HOST = "192.168.1.204";
const char* DEVICE = "esp32-env";
const uint16_t INGEST_PORT = 8080;

const uint8_t SDA_PIN = 21;
const uint8_t SCL_PIN = 22;

const uint32_t SAMPLE_MS = 300000;    // one reading every five minutes
const uint32_t FAIL_MS = 60000;       // first retry after a failed cycle
const uint32_t WIFI_MS = 20000;       // per connection attempt
const uint32_t READY_MS = 8000;       // SCD41 single shot takes about five seconds
const uint16_t TIMEOUT_MS = 8000;     // per POST
const uint32_t MIN_SLEEP_MS = 30000;  // never spin while awake

const uint8_t WIFI_TRIES = 2;
const uint8_t PUSH_TRIES = 2;
const uint8_t QUEUE_LEN = 12;  // one hour of backlog held in RTC memory

const uint32_t SESSION_MAGIC = 0x53434631;

SensirionI2cScd4x sensor;

struct Reading {
  uint16_t co2;
  float temp;
  float humidity;
};

struct Entry {
  uint32_t seq;
  Reading reading;
};

struct SessionState {
  uint32_t magic;
  char bootId[17];
  uint32_t seq;       // last sequence number handed out
  uint32_t failures;  // consecutive failed cycles
  uint8_t head;       // index of the oldest queued entry
  uint8_t count;      // queued entries
  Entry queue[QUEUE_LEN];
};

RTC_DATA_ATTR SessionState session;

void startSession() {
  session.magic = SESSION_MAGIC;
  snprintf(session.bootId, sizeof(session.bootId), "%08x%08x", (unsigned)esp_random(), (unsigned)esp_random());
  session.seq = 0;
  session.failures = 0;
  session.head = 0;
  session.count = 0;
}

void goToSleep(uint32_t ms) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
  esp_deep_sleep_start();
}

uint32_t failureDelay() {
  if (session.failures <= 1) return FAIL_MS;
  if (session.failures == 2) return FAIL_MS * 2;
  return SAMPLE_MS;
}

bool initSensor() {
  Wire.begin(SDA_PIN, SCL_PIN);
  sensor.begin(Wire, SCD41_I2C_ADDR_62);

  for (int attempt = 0; attempt < 3; attempt++) {
    sensor.wakeUp();
    delay(50);
    sensor.stopPeriodicMeasurement();
    uint64_t serial = 0;
    if (sensor.getSerialNumber(serial) == NO_ERROR) return true;
    delay(200);
  }
  return false;
}

bool measure(Reading& r) {
  if (sensor.measureSingleShot() != NO_ERROR) return false;

  bool dataReady = false;
  uint32_t start = millis();
  while (!dataReady && millis() - start < READY_MS) {
    delay(200);
    if (sensor.getDataReadyStatus(dataReady) != NO_ERROR) return false;
  }
  if (!dataReady) return false;

  return sensor.readMeasurement(r.co2, r.temp, r.humidity) == NO_ERROR;
}

bool online() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_11dBm);

  for (uint8_t attempt = 0; attempt < WIFI_TRIES; attempt++) {
    if (attempt > 0) {
      WiFi.disconnect(true);
      delay(500);
    }
    WiFi.begin(WIFI_SSID, WIFI_PASS);

    uint32_t start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < WIFI_MS) delay(200);
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("rssi %d\n", WiFi.RSSI());
      return true;
    }
    Serial.printf("wifi attempt %u status %d\n", (unsigned)attempt + 1, (int)WiFi.status());
  }
  return false;
}

void enqueue(const Reading& r) {
  if (session.count == QUEUE_LEN) {
    session.head = (uint8_t)((session.head + 1) % QUEUE_LEN);  // drop the oldest
    session.count--;
  }

  uint8_t slot = (uint8_t)((session.head + session.count) % QUEUE_LEN);
  session.queue[slot].seq = ++session.seq;
  session.queue[slot].reading = r;
  session.count++;
}

bool push(const Entry& e) {
  char body[224];
  int len = snprintf(body, sizeof body,
                     "{\"device\":\"%s\",\"boot\":\"%s\",\"rows\":[{\"seq\":%lu,\"co2\":%u,\"temp\":%.2f,\"humidity\":%.2f}]}",
                     DEVICE, session.bootId, (unsigned long)e.seq, (unsigned)e.reading.co2,
                     e.reading.temp, e.reading.humidity);
  if (len <= 0 || len >= (int)sizeof body) {
    Serial.println("body overflow");
    return false;
  }

  for (uint8_t attempt = 0; attempt < PUSH_TRIES; attempt++) {
    HTTPClient http;
    if (!http.begin(HOST, INGEST_PORT, "/ingest")) {
      Serial.println("http begin failed");
      delay(500);
      continue;
    }

    http.setTimeout(TIMEOUT_MS);
    http.setReuse(false);
    http.addHeader("Authorization", String("Bearer ") + INGEST_TOKEN);
    http.addHeader("Content-Type", "application/json");

    int code = http.POST((uint8_t*)body, (size_t)len);
    String out = code > 0 ? http.getString() : String();
    http.end();

    Serial.printf("push seq %lu -> %d %s\n", (unsigned long)e.seq, code, out.c_str());
    if (code == 202) return true;
    delay(500);
  }
  return false;
}

bool flush() {
  while (session.count > 0) {
    if (!push(session.queue[session.head])) return false;
    session.head = (uint8_t)((session.head + 1) % QUEUE_LEN);
    session.count--;
  }
  return true;
}

void setup() {
  Serial.begin(115200);
  delay(100);
  const uint32_t cycleStart = millis();

  esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
  bool fromSleep = wake == ESP_SLEEP_WAKEUP_TIMER;
  if (!fromSleep || session.magic != SESSION_MAGIC) startSession();

  Serial.printf("boot %s seq %lu queued %u wake %d reset %d\n", session.bootId,
                (unsigned long)session.seq, (unsigned)session.count, (int)wake,
                (int)esp_reset_reason());

  if (session.count < QUEUE_LEN) {
    Reading r = {};
    if (!initSensor()) {
      Serial.println("sensor init failed");
    } else {
      if (!measure(r)) {
        Serial.println("sensor read failed");
      } else {
        Serial.printf("co2 %u temp %.2f humidity %.2f\n", (unsigned)r.co2, r.temp, r.humidity);
        enqueue(r);
      }
      sensor.powerDown();
    }
  } else {
    Serial.println("queue full, sample skipped");
  }

  if (session.count > 0 && online()) flush();

  bool drained = session.count == 0;
  session.failures = drained ? 0 : session.failures + 1;
  if (!drained) {
    Serial.printf("%u still queued, failures %lu\n", (unsigned)session.count,
                  (unsigned long)session.failures);
  }

  uint32_t target = drained ? SAMPLE_MS : failureDelay();
  uint32_t awake = millis() - cycleStart;
  uint32_t sleepMs = target > awake ? target - awake : 0;
  if (sleepMs < MIN_SLEEP_MS) sleepMs = MIN_SLEEP_MS;

  Serial.printf("sleep %lu ms (awake %lu)\n", (unsigned long)sleepMs, (unsigned long)awake);
  goToSleep(sleepMs);
}

void loop() {
}
