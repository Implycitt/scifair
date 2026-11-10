# Code

<details>
<summary><b>Table of Contents</b></summary>

- [What is happening](#what-is-happening)
- [Libraries](#libraries)
- [Constants](#constants)
- [Memory](#memory)
  - [Wake Up!](#wake-up)
- [f(x)](#fx)
  - [The whole loop](#the-whole-loop)
- [The message it sends](#the-message-it-sends)
- [Endpoint](#endpoint)
- [What should a run look like?](#what-should-a-run-look-like)
- [Index](#index)
- [References](#references)

</details>

The purpose of this document is to describe `Code.ino`, which is the file that runs on the microcontroller.

## What is happening?

An ESP32 is a small computer: a processor, a little memory, and a WiFi radio. The board runs one program forever as soon as it gets power.

For this project, we flashed `Code.ino` onto the board. Its whole job is a cycle of five steps:

1. Wake up and check what the last reading saved was.
2. Get sensor data for a reading, and put it in a small queue kept in memory.
3. Try to send everything in the queue to the NAS, oldest first.
4. Remember whether that worked.
5. Sleep for exactly five minutes.

---

## Libraries

Seven libraries are pulled in at the top of the file. Each one is a package of pre-written code, so the sketch does not have to talk to the hardware directly.

| Line                  | Library        | Why it is there                                      |
| --------------------- | -------------- | ---------------------------------------------------- |
| `Wire.h`              | I2C bus        | The two-wire connection the sensor speaks over       |
| `WiFi.h`              | Networking     | Joining the network, reporting connection status     |
| `HTTPClient.h`        | Networking     | Making the `POST` request, with timeouts and headers |
| `esp_system.h`        | Chip internals | why the chip last restarted                          |
| `esp_random.h`        | Chip internals | the hardware random number generator                 |
| `esp_sleep.h`         | Chip internals | The deep sleep commands                              |
| `SensirionI2cScd4x.h` | Sensor driver  | Turns raw sensor bytes into ppm, C and % humidity    |
| `secrets.h`           | Custom header  | Holds `WIFI_SSID`, `WIFI_PASS`, `INGEST_TOKEN`       |

`secrets.h` is kept in a separate file on purpose since it is the only file containing passwords, so it can be left out of version control and swapped without touching the logic. The sketch refers to those names as if they were ordinary variables.

---

## Constants

All of the behaviour is controlled by a handful of constants near the top. Changing one of these changes how the device behaves; nothing else in the file needs to be touched.

| Constant              | Value           | What it controls                                        |
| --------------------- | --------------- | ------------------------------------------------------- |
| `HOST`                | `192.168.1.204` | Which machine to send readings to                       |
| `INGEST_PORT`         | `8080`          | The port the ingest server listens on                   |
| `DEVICE`              | `esp32-env`     | The name this board reports itself as                   |
| `SDA_PIN` / `SCL_PIN` | `21` / `22`     | Which two pins the sensor is wired to                   |
| `SAMPLE_MS`           | `300000`        | Length of one full cycle, 5 minutes with sleep included |
| `FAIL_MS`             | `60000`         | First retry after a failed cycle; 1 minute              |
| `WIFI_MS`             | `20000`         | Longest it will wait for one connection attempt         |
| `READY_MS`            | `8000`          | Longest it will wait for the sensor                     |
| `TIMEOUT_MS`          | `8000`          | Longest it will wait for the server to answer           |
| `MIN_SLEEP_MS`        | `30000`         | Never sleep for less than half a minute                 |
| `WIFI_TRIES`          | `2`             | How many times it tries to join before giving up        |
| `PUSH_TRIES`          | `2`             | How many times it tries to send one queued reading      |
| `QUEUE_LEN`           | `12`            | How many readings it can hold (one hour of backlog)     |

The sleep values are the main trade-off in the whole design. Five minutes gives dense enough data for a graph.

---

## Memory

Wait a minute! Deep sleep!? But how does memory persist over the deep sleep?!?!

When an ESP32 enters deep sleep it **shuts almost everything down**. When it wakes up, the program starts again at the top of `setup()`.

This is a problem, because the device needs to remember four things between naps:

- _Which power-on session is this?_ (the `boot` ID)
- _How many readings have I taken?_ (the `seq` counter)
- _What failed last time, and how often?_ (the `failures` count)
- _What is still waiting to be sent?_ (the queue)

The first two are needed so the server can tell duplicate readings from new ones. If the counter reset to zero on every wake, every reading would look like the first one and the server would throw all but the first away. The last two are what make a failed WiFi connection survivable.

RTC memory is a small region of memory that stays powered during deep sleep. This memory is about 8KiloBytes. It does not get wiped on wake.

```cpp
RTC_DATA_ATTR SessionState session;
```

`RTC_DATA_ATTR` literally means "put this in RTC memory instead of normal memory."

The whole session is put into a single struct, so a wake reads one block of memory and knows everything:

```cpp
struct SessionState {
};
```

That costs roughly 230 bytes out of 8192. This means that it is small enough that doubling `QUEUE_LEN` would still fit.

`Entry` is just a `seq` plus a `Reading`, so a queued item carries its own number:

```cpp
struct Entry {
  uint32_t seq;
  Reading reading;
};
```

The queue is technically a ring and has a fixed size. Two small numbers describe it,

- `head` is where the oldest item sits.
- `count` is how many items there are.

Adding goes at the end and bumps `count`; sending takes from `head` and bumps `head`. When an index reaches the end it wraps to 0.

```cpp
uint8_t slot = (uint8_t)((session.head + session.count) % QUEUE_LEN);
```

A variable-length list needs memory management, and memory management is not efficient on microcontrollers. A ring uses exactly the same bytes forever, never fragments, and cannot fail to allocate.

`head` and `count` are `uint8_t` (0–255) because a 12-slot buffer never needs more. Modulo arithmetic on a byte-sized index is cheap, and the whole struct stays small.

### Wake up!

```cpp
esp_sleep_wakeup_cause_t wake = esp_sleep_get_wakeup_cause();
bool fromSleep = wake == ESP_SLEEP_WAKEUP_TIMER;
if (!fromSleep || session.magic != SESSION_MAGIC) startSession();
```

When the ESP wakes up, we need to know whether we can trust the old memory first before we actually do anything with it.

- **Did we wake from the timer?**
- **Does the magic number match?** RTC memory comes up with _undefined_ contents so we know it's our actual data.

If these two checks pass then we continue.

---

## f(x)

```cpp
void goToSleep(uint32_t ms) {
  WiFi.disconnect(true);
  WiFi.mode(WIFI_OFF);
  Serial.flush();
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000);
  esp_deep_sleep_start();
}
```

- `WiFi.disconnect(true)` and `WiFi.mode(WIFI_OFF)` switch the radio off properly and forget the access point. Deep sleep alone would eventually cut the radio's power, but leaving it associated means a stale connection sits there until the next wake, and some access points hold the association open.
- `Serial.flush()` forces any pending text out of the serial buffer.
- `esp_sleep_enable_timer_wakeup()` takes **microseconds**, not milliseconds, hence the `* 1000`.
- `(uint64_t)` is a cast which makes the maximum 64 bits unsigned (18,446,744,073,709,551,615!!!)

```cpp
bool online() {
  WiFi.mode(WIFI_STA);
  WiFi.persistent(false);
  WiFi.setSleep(true);
  WiFi.setTxPower(WIFI_POWER_11dBm);

  for (uint8_t attempt = 0; attempt < WIFI_TRIES; attempt++) {
    if (attempt > 0) { WiFi.disconnect(true); delay(500); }
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
```

This code turns the wifi on for the ESP and connects it to the network so that it can send the data to the server. There is a loop that just retries the connection up until the `WIFI_TRIES` and then sleeps.

```cpp
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
```

Here we start data collection with some helper functions. Again we have an attempt maximum so that if there are any small hiccups a whole datapoint isn't thrown away. This piece just makes sure that the sensor is ready to actually get the data we need.

```cpp
bool measure(Reading& r) { ... }
```

The SCD41 is a **photoacoustic CO2 sensor**. A small lamp flashes inside a chamber, a microphone listens for how the light is absorbed by the air, and the driver chip converts that into a parts-per-million number. That measurement takes a few seconds of real time and a meaningful amount of power.

```cpp
if (sensor.measureSingleShot() != NO_ERROR) return false;
```

Two modes exist: _periodic_ (measure forever, in the background) and _single shot_ (take exactly one reading, then idle). Single shot is what a sleeping device wants.

```cpp
while (!dataReady && millis() - start < READY_MS) {
  delay(200);
  if (sensor.getDataReadyStatus(dataReady) != NO_ERROR) return false;
}
```

The reading is not instant. We have to ask if it is ready yet every 200 ms for up to eight seconds. The SCD41 needs about five seconds to take the measurements.

```cpp
return sensor.readMeasurement(r.co2, r.temp, r.humidity) == NO_ERROR;
```

Finally the numbers are copied into the `Reading` structure. The caller powers the sensor down afterwards, whatever happened.

```cpp
void enqueue(const Reading& r) {
  if (session.count == QUEUE_LEN) {           // guard: never overrun the array
    session.head = (uint8_t)((session.head + 1) % QUEUE_LEN);
    session.count--;
  }
  uint8_t slot = (uint8_t)((session.head + session.count) % QUEUE_LEN);
  session.queue[slot].seq = ++session.seq;
  session.queue[slot].reading = r;
  session.count++;
}
```

The ring gets inserted. This just queues up the reading so that the sending pipeline can take place and deliver the struct to the server.

```cpp
if (!http.begin(HOST, INGEST_PORT, "/ingest")) { ... }
http.setTimeout(TIMEOUT_MS);
http.setReuse(false);
http.addHeader("Authorization", String("Bearer ") + INGEST_TOKEN);
http.addHeader("Content-Type", "application/json");
```

`http.begin(...)` opens the connection; the two headers describe the request. The `Authorization` one is how the server knows the request is from us. The token in `secrets.h` acts as a password, and the server rejects anything without it.

```cpp
int code = http.POST((uint8_t*)body, (size_t)len);
String out = code > 0 ? http.getString() : String();
http.end();
if (code == 202) return true;
```

`http.POST()` sends the raw bytes and returns the server's status number or a negative number if the connection never happened. `http.getString()` then reads the reply body (`{"accepted": 1}`), which is purely for the serial log. `http.end()` frees the socket; the ESP32 has only a small number of them, and a leaked socket would eventually make every request fail.

Two attempts are made per entry (`PUSH_TRIES`), half a second apart, because a connection that failed once at the TCP level often works immediately afterwards. If both fail, the entry stays queued and untouched.

### The whole loop

```cpp
if (session.count < QUEUE_LEN) {
  Reading r = {};
  if (!initSensor()) {
    Serial.println("sensor init failed");
  } else {
    if (!measure(r)) {
      Serial.println("sensor read failed");
    } else {
      Serial.printf("co2 %u temp %.2f humidity %.2f\n", ...);
      enqueue(r);
    }
    sensor.powerDown();
  }
} else {
  Serial.println("queue full, sample skipped");
}
```

`Reading r = {};` creates an empty structure with every field zeroed which prevents uninitialised garbage from ever being sent.

Sampling happens **while there is room in the queue**, not only when the queue is empty. If the WiFi drops out for ten minutes, the board keeps taking readings and keeps them, so when the link comes back the whole gap is replayed and the graph has no hole. Once an hour of backlog has built up there is no point adding more that could never be sent, so sampling pauses until the queue drains.

Then the send:

```cpp
if (session.count > 0 && online()) flush();
```

`online()` is only called if something is queued. A cycle that has nothing to send never powers up the radio at all. The radio is the most expensive thing on the board, so this one condition saves lots of battery.

And the ending, which is where the five-minute alignment lives:

```cpp
bool drained = session.count == 0;
session.failures = drained ? 0 : session.failures + 1;

uint32_t target = drained ? SAMPLE_MS : failureDelay();
uint32_t awake = millis() - cycleStart;
uint32_t sleepMs = target > awake ? target - awake : 0;
if (sleepMs < MIN_SLEEP_MS) sleepMs = MIN_SLEEP_MS;

goToSleep(sleepMs);
```

---

## The message it sends

The payload is built like shown below. It must match what the ingest script on the server expects to be accepted.

```json
{
  "device": "esp32-env",
  "boot": "a1b2c3d4e5f60718",
  "rows": [{ "seq": 4, "co2": 812, "temp": 21.5, "humidity": 40.2 }]
}
```

The device never sends a timestamp because it has no real clock.

---

## Endpoint

```cpp
const char* HOST = "192.168.1.204";
```

The endpoint explanation can be found in the [Data folder](../Data/Data.md)

---

## What should a run look like?

```
boot 66d05e55e83b25e2 seq 2 queued 0 wake 4 reset 8
co2 1293 temp 22.76 humidity 61.89
rssi -58
push seq 2 -> 202 {"accepted":1}
sleep 287512 ms (awake 12488)
```

---

## Index

| Word                        | Meaning                                                                                   |
| --------------------------- | ----------------------------------------------------------------------------------------- |
| **I2C**                     | A two-wire way for chips to talk: one data line (SDA), one clock line (SCL)               |
| **Deep sleep**              | A near-total power-down; the chip restarts from the beginning when it wakes               |
| **RTC memory**              | The small memory region that stays powered through deep sleep                             |
| **Boot ID**                 | A label for one power-on session, so the server can tell sessions apart                   |
| **Sequence number (`seq`)** | A number handed out to each reading when it is measured, so duplicates are visible        |
| **Ring buffer**             | A fixed-size queue whose end wraps around to its start; same bytes forever, no allocation |
| **Backoff**                 | Waiting longer after each repeated failure, instead of retrying at a fixed rate           |
| **Brownout**                | A supply voltage dip deep enough to reset the chip (`reset 9`)                            |
| **Magic value**             | A specific nonsense number stored in memory, used to prove the contents are ours          |
| **Idempotent**              | Receiving the same message twice has the same effect as receiving it once                 |
| **HTTP POST**               | The standard way to _send_ data to a server                                               |
| **HTTP status**             | A three-digit answer to a request: `202` accepted, `401` denied, `400` malformed          |
| **Bearer token**            | A password sent in a header, proving the sender is allowed to write                       |
| **JSON**                    | A plain-text data format: curly braces for objects, square brackets for lists             |
| **Serial monitor**          | The text console you watch on your laptop while the board runs, over USB                  |
| **Firmware**                | The program stored on the microcontroller                                                 |

---

## References

- [SCD41 product page and datasheet](https://sensirion.com/products/catalog/SCD41)
- [Sensirion Arduino I2C driver](https://github.com/Sensirion/arduino-i2c-scd4x)
- [ESP32 deep sleep (ESP-IDF docs)](https://docs.espressif.com/projects/esp-idf/en/latest/esp32/api-reference/system/sleep_modes.html)
- [ESP32 `HTTPClient` (Arduino core)](https://docs.espressif.com/projects/arduino-esp32/en/latest/api/http_client.html)
- [Arduino `Wire` library](https://docs.arduino.cc/language-reference/en/functions/communication/wire/)
