/*  Genesis Monitor — Axiometa Genesis Mini
 *
 *  Environment monitor with a five-program menu, MQTT reporting, threshold
 *  alarms and an hourly heartbeat. The stock Axiometa Kit Demo is a menu of
 *  seven programs, four of them games; this keeps the menu and replaces the
 *  toys with instrumentation.
 *
 *    port 1  sliding potentiometer   choose a program, edit a value
 *    port 2  ST7735S 160x80 IPS      the display for every program
 *    port 3  DHT11                   temperature and humidity
 *    port 4  passive buzzer          alarms and the hourly chirp
 *
 *  FILE MAP
 *    pins.h            every pin, MEASURED on this unit -- read its warnings
 *    globals.h         settings, limits, programs, alarm vocabulary
 *    config_manager.h  NVS, and the only place settings are written
 *    detectors.h       when something has actually changed, and why
 *    screens.h         what each program draws; reads nothing, changes nothing
 *
 *  BUILDING
 *  esp32:esp32:esp32s3 + CDCOnBoot=cdc,PSRAM=enabled,FlashSize=4M. The board's
 *  own variant exists only in core 3.x and this estate pins 2.0.17 with
 *  unversioned FQBNs, so installing 3.x would move every other target onto it.
 */
#include <WiFi.h>
#include <PubSubClient.h>
#include <SPI.h>
#include <Wire.h>
#include <DHT.h>
#include <esp_sleep.h>
#include "node_health.h"          // ESP32Eco — retry instead of halting, watchdog
#include "pins.h"
#include "globals.h"
#include "config_manager.h"
#include "detectors.h"
#include "buzzer.h"
#include "annunciator.h"
#include "signage.h"
#include "store.h"
#include "screens.h"
#include "portal.h"

static Settings  cfg;
static Readings  live;
static Detectors detectors;

static Adafruit_ST7735 tft(LCD_CS, LCD_DC, LCD_RST);
static Screens   screens(tft);
static DHT       dht(DHT_PIN, DHT11);
static WiFiClient net;
static PubSubClient mqtt(net);

static char devEui[13] = "";
static int  program = P_AVAIL;
static bool editing = false;

// When something last happened. "Nothing happening" is the condition for deep
// sleep, so it needs a definition: a button press, a serial command, the setup
// portal being open, an alarm, or a change of program. Deliberately NOT the
// sensor loop or an MQTT publish -- those run forever on their own, and if
// they counted as activity the device would never sleep at all.
static uint32_t lastActivity = 0;

// ── Wall clock ──────────────────────────────────────────────────────────────
// The device had no clock at all. Tolerable while every reading was published
// the instant it was taken and stamped on receipt by the back-end -- but it
// makes store-and-forward impossible to do correctly: a buffered outage would
// flush with every reading stamped at flush time, a history that is
// confidently wrong rather than honestly absent. So the clock comes FIRST.
//
// UTC only. No timezone, no DST: a timestamp that travels to a database is an
// instant, and rendering it locally is the display's job. Getting that
// backwards is how two readings an hour apart look simultaneous twice a year.
static bool timeIsValid() {
  // 1 600 000 000 is Sept 2020. The ESP32 boots at epoch 0, so a plausible
  // floor is needed -- testing against 0 alone accepts a half-set clock.
  return time(nullptr) > 1600000000L;
}

// Local wall clock as HH:MM, for the panel only. "--:--" while unset, which is
// honest: a sign that shows a plausible wrong time is worse than one that
// admits it does not know.
static void clockHHMM(char *out, size_t n) {
  if (!timeIsValid()) { snprintf(out, n, "--:--"); return; }
  time_t now = time(nullptr);
  struct tm t;
  localtime_r(&now, &t);            // localtime_r, not gmtime_r: display is local
  snprintf(out, n, "%02d:%02d", t.tm_hour, t.tm_min);
}

static void timeBegin() {
  static bool asked = false;
  if (asked) return;
  asked = true;
  // Two servers: a pool that resolves anywhere, and a fallback for networks
  // that block the pool but allow Google.
  configTime(0, 0, "pool.ntp.org", "time.google.com");
  // configTime with zero offsets keeps time(nullptr) as UTC epoch; TZ affects
  // localtime_r only, which is exactly the split we want.
  setenv("TZ", DISPLAY_TZ, 1);
  tzset();
  Serial.printf("#TIME,SNTP requested (UTC, display %s)\n", DISPLAY_TZ);
}
// A secret held on the panel for a short while, so it can be written down
// without ever reaching a serial log. Cleared when it expires.
static char     secretShown[16] = "";
static bool     secretShowing = false;
static void noteActivity() { lastActivity = millis(); }
static Annunciator ann;
static Signage sign;
static Store   store;
static unsigned long lastRead = 0, lastHist = 0, lastPublish = 0, lastFlush = 0;
// Counters that make an outage visible. A broker down for six hours currently
// looks identical to one that is fine -- the only tell is mqtt=0, and nobody
// is watching that.
static uint32_t reconnects = 0, lastPubOk = 0;
static unsigned long lastDraw = 0, lastHeartbeat = 0, lastEvent = 0;
static int holdPercent = 0;        // progress of a USER hold, 0-100

RTC_DATA_ATTR static uint32_t wakeCount = 0;    // survives deep sleep

// ── Buzzer ──────────────────────────────────────────────────────────────────
// tone() reports "LEDC is not initialized" on core 2.0.17 -- measured on this
// board. Drive LEDC directly; do not swap it back.
static Buzzer buzzer;

// Kept as a name because it reads well at the call sites, but it no longer
// blocks and no longer touches LEDC: the buzzer owns the pin. The old version
// spent ms milliseconds in delay() on every UI click.
static void beep(uint16_t hz, uint16_t ms) { buzzer.note(hz, ms); }
// Routed through the annunciator so one NeoPixel instance owns each strip;
// neopixelWrite() and Adafruit_NeoPixel contend for RMT channels.
static void pixel(uint8_t r, uint8_t g, uint8_t b) { ann.onboard(r, g, b); }

// ── Slider ──────────────────────────────────────────────────────────────────
// USE_SLIDER 0 hands program selection to the USER button (see loop()). The
// slide potentiometer stays supported, not deleted: set this to 1 and reseat
// the module to get the wiper back, unchanged.
#define USE_SLIDER 0

static int sliderRaw() {
  int v[7];
  for (int i = 0; i < 7; i++) { v[i] = analogRead(SLIDER_PIN); delayMicroseconds(80); }
  for (int i = 1; i < 7; i++) {                 // median of 7
    int k = v[i], j = i - 1;
    while (j >= 0 && v[j] > k) { v[j+1] = v[j]; j--; }
    v[j+1] = k;
  }
  return v[3];
}

// Zones with hysteresis, or the program flickers whenever the slider is
// brushed. The first call adopts wherever the slider already sits: without
// that, a slider resting at either extreme could never leave zone 0, because
// the anti-flicker guard also blocked the first move.
#if USE_SLIDER
static int sliderZone(int zones) {
  static int held = -1;
  const int span = 4096 / zones;
  int raw = sliderRaw();
  int z = raw / span;
  if (z >= zones) z = zones - 1;
  if (held < 0) { held = z; return held; }
  int edge = raw - z * span;
  bool clear = edge > span / 5 && edge < span - span / 5;
  if (z != held && (clear || z == 0 || z == zones - 1)) held = z;
  return held;
}
#endif

// Hooks handed to portal.h so it can read and set signage without knowing what
// Signage is. The sketch owns the state; the portal owns the HTML.
static void signApplyHook(const char *avail, const char *note) {
  sign.set(availFromText(avail), note);
}
static void signMessageHook(const char *text) {
  ann.scrollText(text, 0, 60, 30, 0, 2);
}
static const char *signReadHook(const char **note) {
  *note = sign.note();
  return sign.name();
}
static bool signAuthHook(const char *token) {
  return sign.passwordMatches(token, cfg.apPass);
}
static void signSetPassHook(const char *pw) { sign.setPassword(pw); }

// Everything the web page shows that is not signage. Written as JSON fragment
// fields so portal.h never has to know what a Readings is.
//
// NaN is emitted as null rather than 0: a failed sensor read must not arrive
// at a browser looking like a measurement of zero, which is the same mistake
// that put 0 %LEL on a gas display elsewhere in this estate.
static void signStatsHook(char *out, size_t n) {
  char t[24], rh[24];
  if (isnan(live.tempC))    strcpy(t, "null");
  else snprintf(t, sizeof(t), "%.1f", live.tempC);
  if (isnan(live.humidity)) strcpy(rh, "null");
  else snprintf(rh, sizeof(rh), "%.1f", live.humidity);
  snprintf(out, n,
           "\"t\":%s,\"rh\":%s,\"alarm\":\"%s\",\"rhmax\":%.0f,"
           "\"mqtt\":%s,\"up\":%lu,\"muted\":%s,\"eui\":\"%s\","
           "\"queued\":%lu,\"dropped\":%lu,\"lastpub\":%lu",
           t, rh, live.alarm == ALARM_NONE ? "none" : alarmText(live.alarm),
           cfg.rhMax, mqtt.connected() ? "true" : "false",
           millis() / 1000, buzzer.muted() ? "true" : "false", devEui,
           (unsigned long)store.pending(), (unsigned long)store.dropped(),
           (unsigned long)lastPubOk);
}

// ── MQTT ────────────────────────────────────────────────────────────────────
// Expands {eui} in the configured topic template. One template works across a
// fleet: every unit publishes to its own topic without being configured
// individually, which is the only way a topic setting is usable at scale.
static void topicFor(char *out, size_t n, const char *tmpl, const char *suffix) {
  size_t o = 0;
  for (const char *p = tmpl; *p && o + 1 < n; ) {
    if (!strncmp(p, "{eui}", 5)) {
      o += snprintf(out + o, n - o, "%s", devEui);
      p += 5;
    } else {
      out[o++] = *p++;
    }
  }
  if (suffix && o + 1 < n) o += snprintf(out + o, n - o, "%s", suffix);
  out[o < n ? o : n - 1] = '\0';
}

// Sibling of the data topic, not a child of it.
//
// The configured topic names the DATA stream, so events, availability and
// commands belong beside it -- sensors/<eui>/event, not sensors/<eui>/data/event. The
// original code had this right with two hard-coded strings; making the topic
// configurable and appending a suffix to the whole thing quietly nested them,
// and the command topic came out as ".../data/cmd", which is the sort of shape
// somebody has to work around for ever once a dashboard depends on it.
static void siblingTopic(char *out, size_t n, const char *tmpl, const char *leaf) {
  topicFor(out, n, tmpl, nullptr);
  char *slash = strrchr(out, '/');
  if (slash) *slash = '\0';                      // drop the last segment
  size_t o = strlen(out);
  snprintf(out + o, n - o, "/%s", leaf);
}

// Retained: the broker hands the last value to anything connecting later, so a
// dashboard opened on Monday shows the truth rather than nothing. The cost is
// that a stale value outlives a dead device -- hence `up`, which lets a
// consumer tell a current IN from one left over from last week.
static void publishAvail() {
  if (!mqtt.connected()) return;
  char topic[96], payload[192];
  siblingTopic(topic, sizeof(topic), cfg.pubTopic, "avail");
  snprintf(payload, sizeof(payload),
           "{\"avail\":\"%s\",\"note\":\"%s\",\"eui\":\"%s\",\"up\":%lu}",
           sign.name(), sign.note(), devEui, millis() / 1000);
  mqtt.publish(topic, payload, true);
}

// Publishes ONE reading, with the timestamp it was taken at.
//
// The timestamp is a parameter, not time(nullptr), because a record replayed
// out of the flash queue must carry the instant it was MEASURED. Reading the
// clock in here would stamp a flushed outage with the flush time and produce a
// history that is confidently wrong -- the whole reason the clock was added
// before the buffer.
//
// Returns false when the broker would not take it, which is what the queue
// keys off.
static bool publishReading(uint32_t ts, float t, float rh, int rssi) {
  if (!mqtt.connected()) return false;
  char topic[96], payload[224];

  char tsf[32] = "";
  if (ts) snprintf(tsf, sizeof(tsf), ",\"ts\":%lu", (unsigned long)ts);

  topicFor(topic, sizeof(topic), cfg.pubTopic, nullptr);
  snprintf(payload, sizeof(payload),
           "{\"t\":%.1f,\"rh\":%.1f,\"rssi\":%d,\"wake\":%u%s}",
           t, rh, rssi, (unsigned)wakeCount, tsf);
  if (!mqtt.publish(topic, payload)) return false;
  lastPubOk = timeIsValid() ? (uint32_t)time(nullptr) : 0;

  // ── Back-end ingest ─────────────────────────────────────────────────────────
  // A SECOND publish, in the back-end's shape, to its own topic -- not a
  // rename of the one above. That topic is this estate's namespace with its
  // own consumers; the back-end's is a contract owned by somebody else, and
  // one shape serving two masters breaks the first time either moves.
  //
  // general_IOT subscribes to applications/#, requires devEUI and deviceType
  // at the TOP level, looks the device up in its Device table and dispatches
  // to decoders[type]. "dht11" is deliberate: that decoder already exists, is
  // six lines, and reads message.temperature and message.humidity.
  //
  // Empty topic means disabled, and that is the default.
  if (cfg.ingestTopic[0]) {
    char itopic[96], ipayload[256];
    topicFor(itopic, sizeof(itopic), cfg.ingestTopic, nullptr);
    snprintf(ipayload, sizeof(ipayload),
             "{\"devEUI\":\"%s\",\"deviceType\":\"dht11\","
             "\"temperature\":%.1f,\"humidity\":%.1f,\"rssi\":%d%s}",
             devEui, t, rh, rssi, tsf);
    mqtt.publish(itopic, ipayload);
  }
  return true;
}

// The sink Store::flush() hands records to.
static bool replayRecord(const QRec &r) {
  return publishReading(r.ts, r.t, r.rh, r.rssi);
}

static void publish(bool event) {
  if (event) {
    if (!mqtt.connected()) return;      // events are not queued: see below
    char topic[96], payload[224];
    siblingTopic(topic, sizeof(topic), cfg.pubTopic, "event");
    snprintf(payload, sizeof(payload), "{\"kind\":\"%s\",\"t\":%.1f,\"rh\":%.1f}",
             alarmText(live.alarm), live.tempC, live.humidity);
    mqtt.publish(topic, payload);
    return;
  }

  // A reading with no clock is not queued. An unstamped record cannot be
  // replayed honestly, and inventing a timestamp for it would poison the
  // series it is being saved into.
  uint32_t ts = timeIsValid() ? (uint32_t)time(nullptr) : 0;

  if (publishReading(ts, live.tempC, live.humidity, (int)WiFi.RSSI())) return;

  // Broker unavailable. Keep it, if it can be kept honestly.
  if (ts && store.ready()) {
    QRec r{ts, live.tempC, live.humidity, (int16_t)WiFi.RSSI(), 0};
    store.push(r);
  }
}

static void networkService() {
  if (cfg.ssid[0] == '\0') return;
  // While the setup AP is open, stop trying to join the stored network.
  //
  // WiFi.begin() aborts an in-progress scan, so the reconnect timer below was
  // racing the portal's network scan every 15 s and winning often enough that
  // the list came back empty. The portal is open BECAUSE the stored
  // credentials do not work, so retrying them during setup is pure downside.
  if (portalRunning()) return;
  static unsigned long lastWifi = 0, lastMqtt = 0;
  if (WiFi.status() != WL_CONNECTED) {
    if (millis() - lastWifi > 15000) { lastWifi = millis(); WiFi.begin(cfg.ssid, cfg.pass); }
    return;
  }
  // The sign server comes up as soon as the device has an address, and is
  // independent of the broker: on the office network a browser is the direct
  // route and MQTT is for automation. Before this existed the only remote
  // control was the broker, which made a working sign depend on a working
  // broker for no reason.
  timeBegin();                 // once, as soon as there is a network

  static bool signUp = false;
  if (!signUp) {
    signUp = true;
    portalHooks(signApplyHook, signMessageHook, signReadHook, signAuthHook,
                signSetPassHook, signStatsHook);
    portalBeginStation(cfg, devEui, MDNS_NAME_DEFAULT);
  }

  if (!mqtt.connected()) {
    // Back off, because a failed connect BLOCKS.
    //
    // PubSubClient::connect() performs a synchronous TCP connect, and against
    // an unreachable broker that sits on the socket timeout -- roughly fifteen
    // seconds. Retrying every ten seconds therefore left the main loop stalled
    // most of the time: measured during a simulated outage, the device did not
    // answer a single serial command for four minutes, and the sign, the
    // button and the web page were equally frozen.
    //
    // A broker outage must degrade the data path only. Everything the device
    // does locally has to keep working, and that means not spending the loop
    // inside a doomed connect. Ten seconds, doubling to five minutes.
    static uint32_t backoff = 10000;
    if (millis() - lastMqtt > backoff) {
      lastMqtt = millis();
      mqtt.setServer(cfg.mqttHost, cfg.mqttPort);
      // An empty user means connect anonymously; PubSubClient treats a NULL
      // user that way, and passing an empty string instead is NOT the same --
      // some brokers reject it.
      bool up = cfg.mqttUser[0] ? mqtt.connect(devEui, cfg.mqttUser, cfg.mqttPass)
                                : mqtt.connect(devEui);
      backoff = up ? 10000 : (backoff < 300000 ? backoff * 2 : 300000);
      if (up) {
        reconnects++;
        char topic[96];
        siblingTopic(topic, sizeof(topic), cfg.pubTopic, "cmd");
        mqtt.subscribe(topic);
        Serial.printf("#MQTT,subscribed %s\n", topic);
        publishAvail();               // re-assert after any reconnect
      }
    }
    return;
  }
  mqtt.loop();
}

// Commands arriving over MQTT run through the SAME handler as the serial ones.
// One vocabulary, two transports: a second command set for the network would
// drift from this one the first time either gained a verb.
//
// The deny-list is the difference. Nothing from the broker may change the
// device's network identity or make its display unreadable, because the broker
// is reachable by anything on the network and the device is bolted to a window.
static bool mqttCommandAllowed(const String &c) {
  static const char *DENIED[] = {"#WIFI", "#PORTAL", "#SIGNPASS",
                                 "#MXMAP", "#MXWIRE",
                                 "#BZPIN", "#BZSCAN", "#BZID", "#BZONE",
                                 "#POTTEST", "#I2C", "#RAWTONE"};
  for (auto d : DENIED)
    if (c.startsWith(d)) return false;
  return c.startsWith("#");
}

static void hostCommand(String cmd);   // defined below; shared with serial

static void mqttCallback(char *topic, uint8_t *payload, unsigned int len) {
  String body;
  for (unsigned int i = 0; i < len && i < 200; i++) body += (char)payload[i];
  body.trim();
  if (!body.length()) return;
  if (!body.startsWith("#")) body = "#" + body;   // bare "AVAIL,out" also works
  if (!mqttCommandAllowed(body)) {
    Serial.printf("#MQTT,refused over the network: %s\n", body.c_str());
    return;
  }
  Serial.printf("#MQTT,cmd %s\n", body.c_str());
  hostCommand(body);
}

// ── Serial ──────────────────────────────────────────────────────────────────
// Everything answerable over USB with no network and no display.
static void hostCommand(String cmd) {
  while (cmd.length() && (cmd[cmd.length()-1]=='\r' || cmd[cmd.length()-1]=='\n'))
    cmd.remove(cmd.length()-1);
  noteActivity();          // somebody is at the other end of the cable

  if (cmd.startsWith("#STATUS")) {
    Serial.printf("#STATUS,eui=%s,prog=%s,t=%.1f,rh=%.1f,alarm=%s,rhmax=%.1f,"
                  "hist=%d,wifi=%d,mqtt=%d,mode=%s,wakes=%u,up=%lu,"
                  "ssid=%s,broker=%s:%u,topic=%s,user=%s,mqttstate=%d (%s),"
                  "idle=%lus of %us\n",
                  devEui, PROGRAM_NAME[program], live.tempC, live.humidity,
                  alarmText(live.alarm), cfg.rhMax, detectors.historyCount(),
                  WiFi.status()==WL_CONNECTED, mqtt.connected(),
                  cfg.batteryMode ? "battery" : "mains",
                  (unsigned)wakeCount, millis()/1000,
                  cfg.ssid[0] ? cfg.ssid : "(none)", cfg.mqttHost,
                  (unsigned)cfg.mqttPort, cfg.pubTopic,
                  cfg.mqttUser[0] ? cfg.mqttUser : "(anonymous)",
                  mqtt.state(), mqttStateText(mqtt.state()),
                  (unsigned long)((millis() - lastActivity) / 1000),
                  (unsigned)cfg.idleS);
    return;
  }
  if (cmd.startsWith("#READ")) {
    float t = dht.readTemperature(), h = dht.readHumidity();
    if (isnan(t) || isnan(h)) Serial.println("#ERR,sensor did not answer");
    else Serial.printf("#READ,t=%.1f,rh=%.1f\n", t, h);
    return;
  }
  // Kept live even with USE_SLIDER 0: this is how you check a reseated module
  // without reflashing. A healthy wiper sweeps the full 0..4095; the port-3
  // fault reads a flat 0 at every position.
  if (cmd.startsWith("#SLIDER")) {
#if USE_SLIDER
    Serial.printf("#SLIDER,raw=%d,zone=%d\n", sliderRaw(), sliderZone(P_COUNT));
#else
    Serial.printf("#SLIDER,raw=%d,zone=off (USE_SLIDER 0, button drives menu)\n",
                  sliderRaw());
#endif
    return;
  }
  // Drive the buzzer pin directly, bypassing the Buzzer class entirely, to
  // separate "the code is wrong" from "the pin is wrong". Also toggles the pin
  // as a plain output afterwards: an ACTIVE buzzer sounds on DC and ignores
  // PWM frequency, a PASSIVE one is silent on DC and follows the tone.
  // Which of port 1's three lines does the buzzer actually sit on? Drive each
  // with a tone and listen. The loading probe cannot answer this -- a piezo is
  // effectively open circuit to a pull-up test, exactly like the matrix was.
  if (cmd.startsWith("#BZSCAN")) {
    static const uint8_t TRY[3] = {P1_IO0, P1_IO1, P1_IO2};
    for (int i = 0; i < 3; i++) {
      Serial.printf("#BZSCAN,p1_io%d = gpio%-2u (2.7 kHz, 3 s)\n", i, TRY[i]);
      Serial.flush();
      ledcSetup(3, 2700, 10);
      ledcAttachPin(TRY[i], 3);
      ledcWrite(3, 500);
      for (int t = 0; t < 30; t++) { nodeWatchdogFeed(); delay(100); }
      ledcWrite(3, 0);
      ledcDetachPin(TRY[i]);
      pinMode(TRY[i], INPUT);
      delay(300);
    }
    buzzer.begin();
    Serial.println("#BZSCAN,done - set the one that sounded with #BZPIN,<gpio>");
    return;
  }
  // Same fix as colour-coding the matrix scan: the answer must not depend on
  // the listener matching a sound to a moment. Each candidate pin gets its own
  // COUNT of beeps -- one for IO0, two for IO1, three for IO2 -- so "I heard
  // two" identifies the line on its own, minutes later if need be.
  //
  // Drives the pins directly and so ignores mute, which is deliberate: this is
  // the test you run when you cannot hear anything.
  // #BZONE,<gpio>[,seconds] -- drive ONE pin, continuously, and ask a single
  // yes/no question. Two multi-pin tests in a row produced an answer the
  // listener was not sure of; an ambiguous measurement is not a measurement,
  // and three certain answers beat one uncertain one.
  // #CHIME[,<total ms>][,<strum ms>] -- tune the startup chord by ear without
  // reflashing. Shape work needs a fast loop; a two-minute build between every
  // adjustment is how a sound ends up "good enough" instead of right.
  if (cmd.startsWith("#CHIME")) {
    uint16_t total = CHIME_MS, step = 11;
    if (cmd.startsWith("#CHIME,")) {
      String a = cmd.substring(7);
      int c = a.indexOf(',');
      total = (c < 0 ? a : a.substring(0, c)).toInt();
      if (c >= 0) step = a.substring(c + 1).toInt();
      if (total < 200 || total > 8000) total = CHIME_MS;
      if (step < 3 || step > 60) step = 11;
    }
    buzzer.chord(CHIME_GB_MAJOR, 3, total, step);
    Serial.printf("#CHIME,total=%u ms,strum=%u ms\n", total, step);
    return;
  }
  if (cmd.startsWith("#BZONE,")) {
    String a = cmd.substring(7);
    int c = a.indexOf(',');
    uint8_t gpio = (c < 0 ? a : a.substring(0, c)).toInt();
    uint16_t secs = c < 0 ? 6 : a.substring(c + 1).toInt();
    if (secs < 1 || secs > 30) secs = 6;
    Serial.printf("#BZONE,gpio%u for %u s - SOUND or SILENCE?\n", gpio, secs);
    Serial.flush();
    ledcSetup(3, 2700, 10);
    ledcAttachPin(gpio, 3);
    ledcWrite(3, 600);
    for (uint16_t t = 0; t < secs * 10; t++) { nodeWatchdogFeed(); delay(100); }
    ledcWrite(3, 0);
    ledcDetachPin(gpio);
    pinMode(gpio, INPUT);
    buzzer.begin();
    Serial.printf("#BZONE,gpio%u done\n", gpio);
    return;
  }
  if (cmd.startsWith("#BZID")) {
    static const uint8_t TRY[3] = {P1_IO0, P1_IO1, P1_IO2};
    for (int i = 0; i < 3; i++) {
      Serial.printf("#BZID,p1_io%d = gpio%-2u -> %d beep%s\n",
                    i, TRY[i], i + 1, i ? "s" : "");
      Serial.flush();
      ledcSetup(3, 2700, 10);
      ledcAttachPin(TRY[i], 3);
      for (int b = 0; b <= i; b++) {
        ledcWrite(3, 600);
        for (int t = 0; t < 4; t++) { nodeWatchdogFeed(); delay(100); }
        ledcWrite(3, 0);
        for (int t = 0; t < 3; t++) { nodeWatchdogFeed(); delay(100); }
      }
      ledcDetachPin(TRY[i]);
      pinMode(TRY[i], INPUT);
      for (int t = 0; t < 18; t++) { nodeWatchdogFeed(); delay(100); }  // clear gap
    }
    buzzer.begin();
    Serial.println("#BZID,done - how many beeps did you hear? "
                   "1=gpio4  2=gpio3  3=gpio2");
    return;
  }
  if (cmd.startsWith("#BZPIN,")) {
    buzzer.setPin(cmd.substring(7).toInt());
    Buzzer::startupChime(buzzer);
    Serial.printf("#BZPIN,gpio%u (saved)\n", buzzer.pin());
    return;
  }
  if (cmd.startsWith("#BZPIN")) {
    Serial.printf("#BZPIN,gpio%u\n", buzzer.pin());
    return;
  }
  if (cmd.startsWith("#RAWTONE")) {
    Serial.printf("#RAWTONE,driving gpio%d directly\n", BUZZER_PIN);
    ledcSetup(3, 2700, 10);
    ledcAttachPin(BUZZER_PIN, 3);
    for (int i = 0; i < 3; i++) {
      Serial.printf("#RAWTONE,ledc 2700 Hz duty %d/1023\n", 200 + i * 300);
      Serial.flush();
      ledcWrite(3, 200 + i * 300);
      for (int t = 0; t < 15; t++) { nodeWatchdogFeed(); delay(100); }
    }
    ledcWrite(3, 0);
    ledcDetachPin(BUZZER_PIN);

    Serial.println("#RAWTONE,now plain DC HIGH for 2 s (active buzzer only)");
    Serial.flush();
    pinMode(BUZZER_PIN, OUTPUT);
    digitalWrite(BUZZER_PIN, HIGH);
    for (int t = 0; t < 20; t++) { nodeWatchdogFeed(); delay(100); }
    digitalWrite(BUZZER_PIN, LOW);

    Serial.println("#RAWTONE,now bit-banged 2700 Hz square for 2 s");
    Serial.flush();
    uint32_t end = millis() + 2000;
    while ((int32_t)(millis() - end) < 0) {
      digitalWrite(BUZZER_PIN, HIGH); delayMicroseconds(185);
      digitalWrite(BUZZER_PIN, LOW);  delayMicroseconds(185);
    }
    nodeWatchdogFeed();

    buzzer.begin();                    // put the class back in charge
    Serial.println("#RAWTONE,done");
    return;
  }
  if (cmd.startsWith("#BEEP")) { beep(2700, 150); Serial.println("#BEEP,ok"); return; }

  // Drive any annunciator pattern on the bench. Waiting for real weather to
  // see whether a glyph is upside down is not a test strategy.
  // #SIG,<name>[,<seconds>] -- the hold is settable because the default 30 s
  // expires while the person who asked for the pattern is still walking over
  // to look at it.
  if (cmd.startsWith("#SIG,")) {
    String w = cmd.substring(5);
    uint32_t holdS = 30;
    int comma = w.indexOf(',');
    if (comma > 0) { holdS = w.substring(comma + 1).toInt(); w = w.substring(0, comma); }
    if (holdS < 1) holdS = 1;
    Annunciator::Signal s = Annunciator::SIG_IDLE;
    if      (w.startsWith("rh"))   s = Annunciator::SIG_RH_HIGH;
    else if (w.startsWith("temp")) s = Annunciator::SIG_TEMP_RISE;
    else if (w.startsWith("dead")) s = Annunciator::SIG_SENSOR_DEAD;
    else if (w.startsWith("hour")) s = Annunciator::SIG_HOUR;
    else if (w.startsWith("port")) s = Annunciator::SIG_PORTAL;
    else if (w.startsWith("boot")) s = Annunciator::SIG_BOOT;
    ann.setManual(s, holdS * 1000UL);
    Serial.printf("#SIG,%s (held %lu s, then automatic control resumes)\n",
                  Annunciator::name(s), (unsigned long)holdS);
    return;
  }

  // Find the matrix data line by driving each of a port's three IO lines in
  // turn. #PORTZ cannot do this: a WS2812 data input is high-impedance, so it
  // reads "open" whether the module is plugged in or not.
  if (cmd.startsWith("#MXSCAN")) {
    // Each candidate line gets its own COLOUR, not its own numbered step. The
    // first version drove all three red and asked which step lit -- which
    // requires the observer to be watching the exact second, and to have the
    // serial log beside them. "It went green" identifies the pin on its own,
    // needs no timing, and survives being reported minutes later.
    static const uint8_t TRY[3] = {P3_IO0, P3_IO1, P3_IO2};
    static const uint8_t COL[3][3] = {{70, 0, 0}, {0, 70, 0}, {0, 0, 70}};
    static const char *NAME[3] = {"RED", "GREEN", "BLUE"};
    for (int cycle = 0; cycle < 3; cycle++) {
      for (int i = 0; i < 3; i++) {
        Serial.printf("#MXSCAN,p3_io%d = gpio%-2u -> %s\n", i, TRY[i], NAME[i]);
        Serial.flush();
        ann.probePin(TRY[i], COL[i][0], COL[i][1], COL[i][2]);
        for (int t = 0; t < 40; t++) { nodeWatchdogFeed(); delay(100); }
      }
    }
    ann.probeEnd();
    Serial.println("#MXSCAN,done - report the COLOUR the matrix showed: "
                   "red=io0/gpio9  green=io1/gpio16  blue=io2/gpio15");
    return;
  }

  // Identify the panel wiring from three observations instead of guessing
  // among sixteen orientations. Lights raw strip positions, bypassing index()
  // entirely -- a discovery test must not apply the mapping it is discovering.
  if (cmd.startsWith("#MXWIRE")) {
    Serial.println("#MXWIRE,1/3 LED 0 only (red, 6 s): which CORNER lights?");
    Serial.flush();
    ann.probeRaw(0, 0, 70, 0, 0);
    for (int t = 0; t < 60; t++) { nodeWatchdogFeed(); delay(100); }

    Serial.println("#MXWIRE,2/3 LEDs 0-4 (green, 6 s): a ROW or a COLUMN?");
    Serial.flush();
    ann.probeRaw(0, 4, 0, 70, 0);
    for (int t = 0; t < 60; t++) { nodeWatchdogFeed(); delay(100); }

    Serial.println("#MXWIRE,3/3 LEDs 5-9 (blue, 6 s): does the second line "
                   "start at the SAME end as the green one, or the far end?");
    Serial.flush();
    ann.probeRaw(5, 9, 0, 30, 70);
    for (int t = 0; t < 60; t++) { nodeWatchdogFeed(); delay(100); }

    ann.showStill(Annunciator::testGlyph(), 0, 60, 30);
    Serial.println("#MXWIRE,done - same end = progressive, far end = serpentine");
    return;
  }

  // #MXMAP,<originRight>,<originBottom>,<colMajor>,<serpentine>, each 0 or 1.
  if (cmd.startsWith("#MXMAP,")) {
    String a2 = cmd.substring(7);
    int v[4] = {0, 0, 0, 0}, n = 0;
    while (n < 4 && a2.length()) {
      int c = a2.indexOf(',');
      v[n++] = (c < 0 ? a2 : a2.substring(0, c)).toInt() != 0;
      if (c < 0) break;
      a2 = a2.substring(c + 1);
    }
    if (n < 4) { Serial.println("#ERR,usage #MXMAP,<oRight>,<oBottom>,<colMajor>,<serp>"); return; }
    ann.setMapping(v[0], v[1], v[2], v[3]);
    ann.showStill(Annunciator::testGlyph(), 0, 60, 30);
    Serial.printf("#MXMAP,oRight=%d,oBottom=%d,colMajor=%d,serp=%d (saved)\n",
                  ann.originRight(), ann.originBottom(), ann.colMajor(), ann.serpentine());
    return;
  }
  if (cmd.startsWith("#MXMAP")) {
    ann.showStill(Annunciator::testGlyph(), 0, 60, 30);
    Serial.printf("#MXMAP,oRight=%d,oBottom=%d,colMajor=%d,serp=%d (showing test F)\n",
                  ann.originRight(), ann.originBottom(), ann.colMajor(), ann.serpentine());
    return;
  }

  // #MXDIR,<rtl|ltr> -- which way a marquee travels, independent of the panel
  // mapping set by #MXMAP.
  if (cmd.startsWith("#MXDIR,")) {
    String w = cmd.substring(7); w.trim();
    ann.setDirection(!w.startsWith("ltr"));
    ann.scrollText(GREETING_DEFAULT, 0, 60, 30, 0, 1);
    Serial.printf("#MXDIR,%s (saved)\n", ann.rightToLeft() ? "rtl" : "ltr");
    return;
  }
  if (cmd.startsWith("#MXDIR")) {
    Serial.printf("#MXDIR,%s\n", ann.rightToLeft() ? "rtl" : "ltr");
    return;
  }

  // #MXSPEED,<ms per column>,<blank columns between characters>
  if (cmd.startsWith("#MXSPEED,")) {
    String a = cmd.substring(9);
    int c = a.indexOf(',');
    uint16_t ms = a.substring(0, c < 0 ? a.length() : c).toInt();
    uint8_t gap = c < 0 ? ann.marqueeGap() : a.substring(c + 1).toInt();
    ann.setMarquee(ms, gap);
    ann.scrollText(GREETING_DEFAULT, 0, 60, 30, 0, 1);   // show the result
    Serial.printf("#MXSPEED,step=%u ms,gap=%u columns (saved)\n",
                  ann.marqueeStep(), ann.marqueeGap());
    return;
  }
  if (cmd.startsWith("#MXSPEED")) {
    Serial.printf("#MXSPEED,step=%u ms,gap=%u columns\n",
                  ann.marqueeStep(), ann.marqueeGap());
    return;
  }
  // A lab buzzer that cannot be silenced is a lab buzzer that gets unplugged,
  // and an unplugged buzzer is a silent alarm. Mute and volume persist, in
  // their own NVS namespace so they survive a Settings layout change.
  if (cmd.startsWith("#MUTE")) {
    String a = cmd.substring(5); a.replace(",", ""); a.trim();
    if (a.length()) buzzer.setMute(a.startsWith("on") || a == "1");
    Serial.printf("#MUTE,%s\n", buzzer.muted() ? "on" : "off");
    return;
  }
  if (cmd.startsWith("#VOL")) {
    String a = cmd.substring(4); a.replace(",", ""); a.trim();
    if (a.length()) { buzzer.setVolume(a.toInt()); buzzer.note(2700, 120); }
    Serial.printf("#VOL,%u of 10%s\n", buzzer.volume(),
                  buzzer.muted() ? " (MUTED)" : "");
    return;
  }
  if (cmd.startsWith("#TUNE,")) {
    String w = cmd.substring(6);
    if      (w.startsWith("wel"))  buzzer.play(Buzzer::MEL_WELCOME);
    else if (w.startsWith("rh"))   buzzer.play(Buzzer::MEL_RH);
    else if (w.startsWith("temp")) buzzer.play(Buzzer::MEL_TEMP);
    else if (w.startsWith("dead")) buzzer.play(Buzzer::MEL_DEAD);
    else if (w.startsWith("hour")) buzzer.play(Buzzer::MEL_HOUR);
    else if (w.startsWith("sav"))  buzzer.play(Buzzer::MEL_SAVED);
    else if (w.startsWith("star"))   Buzzer::startupChime(buzzer);
    else if (w.startsWith("sleep"))  buzzer.play(Buzzer::MEL_SLEEP);
    else if (w.startsWith("warn"))   buzzer.play(Buzzer::MEL_WARN);
    else if (w.startsWith("heal"))   buzzer.play(Buzzer::MEL_HEALTHY);
    else if (w.startsWith("noconn")) buzzer.play(Buzzer::MEL_NOCONN);
    else { Serial.println("#ERR,tune: startup|sleep|warn|healthy|noconn|"
                          "welcome|rh|temp|dead|hour|saved"); return; }
    Serial.printf("#TUNE,%s%s\n", w.c_str(), buzzer.muted() ? " (MUTED)" : "");
    return;
  }

  // Scroll arbitrary text, for setting up a room or checking the font.
  if (cmd.startsWith("#TEXT,")) {
    String t = cmd.substring(6);
    ann.scrollText(t.c_str(), 0, 60, 30, 0, 2);
    Serial.printf("#TEXT,%s\n", t.c_str());
    return;
  }
  if (cmd.startsWith("#TEXT")) {                  // no argument: the greeting
    ann.scrollText(GREETING_DEFAULT, 0, 60, 30, 0, 3);
    Serial.printf("#TEXT,%s\n", GREETING_DEFAULT);
    return;
  }

  // Port wiring test, kept in the firmware so a module can be checked after
  // reseating without flashing a scratch sketch.
  //
  // A ~45 kohm internal pull-up against whatever the module presents:
  //   pullup ~4095 and pulldown ~0   nothing attached (open)
  //   pullup pulled well down        something attached
  //   both stuck at 0                the line is shorted to ground
  // This is the test that found port 3 shorted. ADC2 pins are read with the
  // radio down where possible; with WiFi up their values are not meaningful,
  // which the output says rather than leaving you to be misled.
  if (cmd.startsWith("#PORTZ,")) {
    int port = cmd.substring(7).toInt();
    static const uint8_t MAP[4][3] = {{P1_IO0, P1_IO1, P1_IO2},
                                      {P2_IO0, P2_IO1, P2_IO2},
                                      {P3_IO0, P3_IO1, P3_IO2},
                                      {P4_IO0, P4_IO1, P4_IO2}};
    if (port < 1 || port > 4) { Serial.println("#ERR,port must be 1-4"); return; }
    bool radio = WiFi.status() == WL_CONNECTED || portalRunning();   // AP or STA
    Serial.printf("#PORTZ,port%d  note: a pin this firmware drives as an OUTPUT "
                  "reads 'open' when idle (buzzer, LCD, matrix data)\n", port);
    for (int i = 0; i < 3; i++) {
      uint8_t g = MAP[port - 1][i];
      pinMode(g, INPUT);          delay(5); int hiZ = analogRead(g);
      pinMode(g, INPUT_PULLUP);   delay(5); int pu  = analogRead(g);
      pinMode(g, INPUT_PULLDOWN); delay(5); int pd  = analogRead(g);
      pinMode(g, INPUT);
      bool adc2 = (g >= 11 && g <= 20);
      // GPIO 3 is the ESP32-S3 JTAG-source strapping pin AND, on this board,
      // port 1 IO1 -- where the buzzer actually sits. When this note was
      // written the load was attributed entirely to the strapping resistor,
      // on the assumption that the buzzer was on IO0; that assumption was
      // wrong, so treat "loaded" here as ambiguous between the two and
      // identify the module by driving it, not by measuring it.
      bool strap = (g == 3);
      // "pulled low" rather than "shorted": an addressable-LED module's data
      // input carries a pull-down that beats the internal pull-up and reads
      // identically to a dead short. This tool called port 3 faulty twice on
      // that basis and was wrong both times. It cannot tell the two apart --
      // only driving the line can, which is what #MXSCAN is for.
      const char *v = (pu > 3600 && pd < 400) ? "open or high-Z input"
                    : (pu < 400 && pd < 400)  ? "pulled low (short, OR a data "
                                                "line's pull-down -- use #MXSCAN)"
                    : (pu < 2000)             ? "loaded"
                                              : "partial load";
      Serial.printf("#PORTZ,p%d_io%d=gpio%-2u hiZ=%-4d pu=%-4d pd=%-4d %s%s%s\n",
                    port, i, g, hiZ, pu, pd, v,
                    strap ? "  (strapping pin: board resistor, not a module)" : "",
                    (adc2 && radio) ? "  (ADC2 with radio up -- not meaningful)" : "");
    }
    return;
  }
  // #QUEUE -- what the buffer is holding, and why.
  if (cmd.startsWith("#QUEUE,clear")) {
    store.clear();
    Serial.println("#QUEUE,cleared");
    return;
  }
  if (cmd.startsWith("#QUEUE")) {
    Serial.printf("#QUEUE,pending=%lu,dropped=%lu,corrupt=%lu,mounted=%d,"
                  "reconnects=%lu,lastpub=%lu,mqtt=%d\n",
                  (unsigned long)store.pending(), (unsigned long)store.dropped(),
                  (unsigned long)store.badRecords(), store.ready(),
                  (unsigned long)reconnects, (unsigned long)lastPubOk,
                  mqtt.state());
    return;
  }

  // #HOUR -- strike the hourly mark now. Without this the only way to check it
  // is to wait up to an hour, which is how an hourly signal goes unverified.
  if (cmd.startsWith("#HOUR")) {
    ann.set(Annunciator::SIG_HOUR);
    Serial.println("#HOUR,struck");
    return;
  }

  // #TIME -- is the clock set, and to what.
  if (cmd.startsWith("#TIME")) {
    time_t now = time(nullptr);
    char buf[32] = "unset";
    if (timeIsValid()) {
      struct tm t;
      gmtime_r(&now, &t);
      strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &t);
    }
    Serial.printf("#TIME,%s,epoch=%ld,valid=%d\n", buf, (long)now, timeIsValid());
    return;
  }

  // #BROKER[,<host>[,<port>]] -- the MQTT broker. Serial as well as portal,
  // because repointing a device should not require joining its access point
  // when there is a cable in it. Applies on the next reconnect.
  if (cmd.startsWith("#BROKER,")) {
    String a = cmd.substring(8); a.trim();
    int c = a.indexOf(',');
    String host = (c < 0 ? a : a.substring(0, c)); host.trim();
    if (host.length()) {
      strncpy(cfg.mqttHost, host.c_str(), sizeof(cfg.mqttHost) - 1);
      cfg.mqttHost[sizeof(cfg.mqttHost) - 1] = '\0';
    }
    if (c >= 0) {
      long pt = a.substring(c + 1).toInt();
      if (pt > 0 && pt < 65536) cfg.mqttPort = (uint16_t)pt;
    }
    settingsSave(cfg);
    mqtt.disconnect();                 // force a reconnect to the new address
    Serial.printf("#BROKER,%s:%u (saved, reconnecting)\n",
                  cfg.mqttHost, (unsigned)cfg.mqttPort);
    return;
  }
  if (cmd.startsWith("#BROKER")) {
    Serial.printf("#BROKER,%s:%u,state=%d (%s)\n", cfg.mqttHost,
                  (unsigned)cfg.mqttPort, mqtt.state(),
                  mqttStateText(mqtt.state()));
    return;
  }

  // #INGEST[,<topic>|off] -- where an ingest back-end receives this device.
  // Blank or "off" disables it. {eui} expands, as in the publish topic.
  if (cmd.startsWith("#INGEST,")) {
    String t = cmd.substring(8); t.trim();
    if (t == "off" || t == "-") t = "";
    strncpy(cfg.ingestTopic, t.c_str(), sizeof(cfg.ingestTopic) - 1);
    cfg.ingestTopic[sizeof(cfg.ingestTopic) - 1] = '\0';
    settingsSave(cfg);
    Serial.printf("#INGEST,%s\n", cfg.ingestTopic[0] ? cfg.ingestTopic : "off");
    return;
  }
  if (cmd.startsWith("#INGEST")) {
    if (!cfg.ingestTopic[0]) { Serial.println("#INGEST,off"); return; }
    char t[96];
    topicFor(t, sizeof(t), cfg.ingestTopic, nullptr);
    Serial.printf("#INGEST,%s -> %s\n", cfg.ingestTopic, t);
    return;
  }

  // #IDLE[,<seconds>] -- how long nothing must happen before it sleeps.
  // The real sizes, from the compiler that built this image.
  if (cmd.startsWith("#CFGSZ")) {
    Serial.printf("#CFGSZ,Settings=%u,V4=%u,V3=%u,V2=%u,V1=%u\n",
                  (unsigned)sizeof(Settings), (unsigned)sizeof(SettingsV4),
                  (unsigned)sizeof(SettingsV3), (unsigned)sizeof(SettingsV2),
                  (unsigned)sizeof(SettingsV1));
    return;
  }
  // #APROTATE -- throw away the AP password and generate a new one.
  //
  // Needed because a disclosed password has no other remedy: it is generated
  // once and then kept for the life of the device. The new one is shown on the
  // screen, never printed here.
  if (cmd.startsWith("#APROTATE")) {
    // apPasswordEnsure() keeps any stored password of usable length, so
    // clearing this field is NOT enough -- it would reload the same value from
    // NVS and report success. Rotation clears the stored key.
    apPasswordRotate(cfg.apPass, sizeof(cfg.apPass));
    settingsSave(cfg);
    Serial.printf("#APROTATE,new password generated (%u characters); "
                  "read it on the SETTINGS screen with the portal open\n",
                  (unsigned)strlen(cfg.apPass));
    if (portalRunning())
      Serial.println("#APROTATE,the running AP still uses the OLD password "
                     "until the device restarts");
    return;
  }

  // #SIGNPASS,<new> -- the web sign's write password. Serial only: it is on the
  // MQTT deny-list, so the broker cannot change it, and it is never printed
  // back. The length is reported instead, which is enough to confirm it took.
  if (cmd.startsWith("#SIGNPASS,")) {
    String pw = cmd.substring(10); pw.trim();
    // A way back. The sign password can be set from a browser form, and a
    // browser can fill that form by itself: password managers autofill fields
    // regardless of autocomplete=off, which left this device holding a
    // credential nobody had chosen and locked its owner out of their own sign.
    if (pw == "clear") {
      sign.setPassword("");
      Serial.println("#SIGNPASS,cleared -- falling back to the AP password");
      return;
    }
    // "gen" generates one and shows it ON THE PANEL, never over serial.
    //
    // Recovery previously required the setup AP, whose own password is read
    // from the same panel -- so a lost sign password meant a two-step dance
    // through a second credential. This is the direct route: hold it on the
    // screen long enough to write down, and nothing reaches a log.
    if (pw == "gen") {
      static const char alphabet[] = "abcdefghijkmnopqrstuvwxyz"
                                     "ABCDEFGHJKLMNPQRSTUVWXYZ23456789";
      char fresh[11];
      for (int i = 0; i < 10; i++)
        fresh[i] = alphabet[esp_random() % (sizeof(alphabet) - 1)];
      fresh[10] = '\0';
      sign.setPassword(fresh);
      strncpy(secretShown, fresh, sizeof(secretShown) - 1);
      // Held until dismissed, not for a fixed window. Ninety seconds expired
      // before it could be used; ten minutes still put a clock on someone
      // walking to another room. The person standing at the device is the one
      // who knows when they are finished with it, and pressing any button says
      // so -- so that is what takes it down.
      secretShowing = true;
      Serial.printf("#SIGNPASS,generated (%u characters) -- ON THE PANEL until "
                    "a button is pressed, not printed here\n",
                    (unsigned)strlen(fresh));
      return;
    }
    if (pw.length() < 6) {
      Serial.println("#ERR,sign password must be at least 6 characters");
      return;
    }
    sign.setPassword(pw.c_str());
    Serial.printf("#SIGNPASS,set (%u characters)\n", sign.passwordLength());
    return;
  }
  if (cmd.startsWith("#SIGNPASS")) {
    Serial.printf("#SIGNPASS,%s\n", sign.hasPassword()
                  ? "set; not shown"
                  : "NOT set -- falling back to the AP password, which is "
                    "displayed on the screen facing the corridor");
    return;
  }

  // #AVAIL,<in|out|busy|class>[,<note>]
  if (cmd.startsWith("#AVAIL,")) {
    String a = cmd.substring(7);
    int c = a.indexOf(',');
    String state = c < 0 ? a : a.substring(0, c);
    String note  = c < 0 ? String("") : a.substring(c + 1);
    sign.set(availFromText(state.c_str()), note.c_str());
    Serial.printf("#AVAIL,%s,%s\n", sign.name(), sign.note());
    return;
  }
  if (cmd.startsWith("#AVAIL")) {
    Serial.printf("#AVAIL,%s,%s\n", sign.name(), sign.note());
    return;
  }
  // #MSG,<text> -- scroll a notice for the corridor.
  if (cmd.startsWith("#MSG,")) {
    String m = cmd.substring(5);
    ann.scrollText(m.c_str(), 0, 60, 30, 0, 2);
    Serial.printf("#MSG,%s\n", m.c_str());
    return;
  }
  if (cmd.startsWith("#IDLE,")) {
    // Zero means never. A window-facing sign that blanks itself is useless,
    // and a sleeping device cannot receive MQTT at all -- the radio is off, so
    // no availability change, message or wake can reach it.
    uint16_t want = (uint16_t)cmd.substring(6).toInt();
    cfg.idleS = want == 0 ? 0
                          : clampTo(want, Limits::IDLE_MIN, Limits::IDLE_MAX);
    settingsSave(cfg);
    Serial.printf("#IDLE,%u s (saved)\n", cfg.idleS);
    return;
  }
  if (cmd.startsWith("#IDLE")) {
    Serial.printf("#IDLE,%u s, idle for %lu s now, mode=%s -> %s\n", cfg.idleS,
                  (unsigned long)((millis() - lastActivity) / 1000),
                  cfg.batteryMode ? "battery" : "mains",
                  (cfg.batteryMode && cfg.idleS) ? "will sleep" : "never sleeps");
    return;
  }
  if (cmd.startsWith("#RHMAX,")) {
    cfg.rhMax = clampTo(cmd.substring(7).toFloat(), Limits::RH_MIN, Limits::RH_MAX);
    settingsSave(cfg);
    Serial.printf("#RHMAX,%.1f\n", cfg.rhMax);
    return;
  }
  if (cmd == "#WIFI") {
    Serial.printf("#WIFI,ssid=%s,pass_len=%u,status=%d\n",
                  cfg.ssid[0] ? cfg.ssid : "(none)",
                  (unsigned)strlen(cfg.pass), WiFi.status());
    return;
  }
  if (cmd.startsWith("#WIFI,")) {
    int c = cmd.indexOf(',', 6);
    if (c < 0) { Serial.println("#ERR,usage #WIFI,<ssid>,<pass>"); return; }
    strncpy(cfg.ssid, cmd.substring(6, c).c_str(), sizeof(cfg.ssid)-1);
    strncpy(cfg.pass, cmd.substring(c+1).c_str(), sizeof(cfg.pass)-1);
    settingsSave(cfg);
    // Length only. The password is never echoed back.
    Serial.printf("#WIFI,ssid=%s,pass_len=%u\n", cfg.ssid, (unsigned)strlen(cfg.pass));
    WiFi.begin(cfg.ssid, cfg.pass);
    return;
  }
  if (cmd.startsWith("#MODE,")) {
    cfg.batteryMode = cmd.substring(6).startsWith("bat");
    settingsSave(cfg);
    Serial.printf("#MODE,%s\n", cfg.batteryMode ? "battery" : "mains");
    return;
  }
  // Current medians for every safe ADC pin. Deliberately NOT min/max: that
  // statistic saturates to 0-4095 on any floating pin given enough samples,
  // and reports every unused pin as a moving signal. GPIO 19 and 20 are
  // omitted -- they are the USB data lines.
  // Does the slider need its divider powered from the port's own GPIOs?
  //
  // Port 1's IO0 (GPIO 4) reads EXACTLY 0 always -- a pin tied to ground, not
  // a floating one, which would drift. A fader wired as a divider needs a top
  // rail; if the module expects one of the port's other lines to supply it,
  // the wiper sits at 0 until that line is driven high. This drives each of
  // the port's pins high in turn and reads the others.
  if (cmd.startsWith("#POTTEST")) {
    static const uint8_t PORTS[4][3] = {
      { 4, 3, 2 }, { 7, 6, 5 }, { 9, 16, 15 }, { 1, 17, 18 }
    };
    int port = 1;
    int comma = cmd.indexOf(',');
    if (comma > 0) port = cmd.substring(comma + 1).toInt();
    if (port < 1 || port > 4) { Serial.println("#ERR,port 1..4"); return; }
    const uint8_t *P1 = PORTS[port - 1];
    Serial.printf("#POTTEST,port=%d,pins=%d/%d/%d\n", port, P1[0], P1[1], P1[2]);
    for (int drive = 0; drive < 3; drive++) {
      for (int i = 0; i < 3; i++) {
        if (i == drive) { pinMode(P1[i], OUTPUT); digitalWrite(P1[i], HIGH); }
        else            { pinMode(P1[i], INPUT); }
      }
      delay(30);
      Serial.printf("#POTTEST,drive=%d_high", P1[drive]);
      for (int i = 0; i < 3; i++)
        if (i != drive) Serial.printf(",gpio%d=%d", P1[i], analogRead(P1[i]));
      Serial.println();
    }
    for (int i = 0; i < 3; i++) pinMode(P1[i], INPUT);   // leave them alone

    // Also try every attenuation. The default is 11 dB; if the module presents
    // a small voltage, a wrong attenuation could floor it -- though it would
    // have to be very small to read exactly 0.
    for (int a = 0; a < 4; a++) {
      analogSetPinAttenuation(P1[0], (adc_attenuation_t)a);
      delay(10);
      Serial.printf("#POTTEST,atten=%d,gpio%d=%d\n", a, P1[0], analogRead(P1[0]));
    }
    analogSetPinAttenuation(P1[0], ADC_11db);
    Serial.println("#POTTEST,done");
    return;
  }
  // Capacitive touch on every touch-capable pin. The ESP32-S3 has touch on
  // GPIO 1-14, and a capacitive slider reads near zero on analogRead while
  // responding only to touchRead -- which would explain a module that the
  // vendor firmware reads happily and this one cannot see at all.
  // Every port wires the same I2C bus (SDA 10 / SCL 11 per the schematic), so
  // an I2C module answers regardless of which port it sits in. Also reports
  // whether anything is pulling the bus up: with no external pull-up there is
  // no I2C device present, whatever a scan returns.
  if (cmd.startsWith("#I2C")) {
    pinMode(10, INPUT); pinMode(11, INPUT); delay(2);
    int sdaFloat = digitalRead(10), sclFloat = digitalRead(11);
    pinMode(10, INPUT_PULLUP); pinMode(11, INPUT_PULLUP); delay(2);
    int sdaPull = digitalRead(10), sclPull = digitalRead(11);
    Serial.printf("#I2C,sda_float=%d,sda_pullup=%d,scl_float=%d,scl_pullup=%d,%s\n",
                  sdaFloat, sdaPull, sclFloat, sclPull,
                  (sdaFloat && sclFloat) ? "external pull-ups present"
                                         : "NO external pull-up - no I2C device");
    Wire.begin(10, 11, 100000);
    delay(30);
    Serial.print("#I2C,found:");
    int n = 0;
    for (uint8_t a = 1; a < 127; a++) {
      Wire.beginTransmission(a);
      if (Wire.endTransmission() == 0) { Serial.printf(" 0x%02X", a); n++; }
    }
    Serial.println(n ? "" : " nothing");
    Wire.end();
    return;
  }
  if (cmd.startsWith("#TOUCH")) {
    Serial.print("#TOUCH");
    for (uint8_t pin = 1; pin <= 14; pin++) {
      if (pin == 12 || pin == 13 || pin == 14) continue;   // SPI to the display
      if (pin == LCD_CS || pin == LCD_DC || pin == LCD_RST) continue;
      Serial.printf(" %d:%lu", pin, (unsigned long)touchRead(pin));
    }
    Serial.println();
    return;
  }
  if (cmd.startsWith("#PINS")) {
    Serial.print("#PINS");
    for (uint8_t pin = 1; pin <= 18; pin++) {
      int v[9];
      for (int i = 0; i < 9; i++) { v[i] = analogRead(pin); delayMicroseconds(60); }
      for (int i = 1; i < 9; i++) {
        int k = v[i], j = i - 1;
        while (j >= 0 && v[j] > k) { v[j+1] = v[j]; j--; }
        v[j+1] = k;
      }
      Serial.printf(" %d:%d", pin, v[4]);
    }
    Serial.println();
    return;
  }
  if (cmd.startsWith("#PORTAL")) {
    portalBegin(cfg, devEui, []() { settingsSave(cfg); });
    Serial.printf("#PORTAL,up,pass=%u characters (on the SETTINGS screen)\n",
                  (unsigned)strlen(portalApPassword()));
    return;
  }
  if (cmd.startsWith("#RESET")) {
    settingsClear();
    Serial.println("#RESET,settings cleared - restarting");
    Serial.flush(); delay(200); ESP.restart();
  }
  if (cmd.startsWith("#")) Serial.println("#ERR,unknown command");
}

static void pollSerial() {
  static String line;
  while (Serial.available()) {
    char c = (char)Serial.read();
    if (c == '\n') { hostCommand(line); line = ""; }
    else if (c != '\r' && line.length() < 140) line += c;
  }
}

// ── Setup ───────────────────────────────────────────────────────────────────
void setup() {
  Serial.begin(115200);
  delay(400);
  wakeCount++;

  uint8_t mac[6];
  WiFi.macAddress(mac);
  snprintf(devEui, sizeof(devEui), "%02X%02X%02X%02X%02X%02X",
           mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

  bool stored = settingsLoad(cfg);
  sign.begin();
  store.begin();
  mqtt.setCallback(mqttCallback);
  buzzer.begin();         // before anything that can make a sound
  ann.attach(&buzzer);
  pinMode(PIN_USER, INPUT_PULLUP);
  pixel(0, 0, 25);

  SPI.begin(PIN_SCK, PIN_MISO, PIN_MOSI, -1);
  tft.initR(INITR_MINI160x80);
  tft.setRotation(3);
  dht.begin();
  detectors.reset();
  ann.loadGeometry();     // before begin(), which paints
  ann.begin();            // after the display: a matrix fault must not be able
                          // to stop the screen from coming up
  // Three passes, ~30 s: long enough that whoever powered the board on gets to
  // read it, and it stands down on its own. An alarm interrupts it.
  ann.scrollText(GREETING_DEFAULT, 0, 60, 30, 0, 3);
  Buzzer::startupChime(buzzer);

  Serial.printf("\n=== Genesis Monitor  eui=%s  wake=%u  settings=%s ===\n",
                devEui, (unsigned)wakeCount, stored ? "stored" : "defaults");
  Serial.println("commands: #STATUS #READ #SLIDER #BEEP #SIG,<x>[,<s>] #TEXT[,<msg>] #AVAIL[,state,note] #MSG,<text> #SIGNPASS,<pw> #APROTATE #HOUR #QUEUE[,clear] #TIME #BROKER[,host,port] #INGEST[,topic] #IDLE[,s] #MUTE[,on|off] #VOL[,0-10] #TUNE,<t> #BZSCAN #BZID #BZONE,<gpio> #CHIME[,ms,strum] #BZPIN[,gpio] #MXWIRE #MXMAP[,r,b,c,s] #MXDIR[,rtl|ltr] #MXSPEED[,ms,gap] #PORTZ,<n> #RHMAX,<v> "
                 "#WIFI,<ssid>,<pass> #MODE,<mains|battery> #RESET");

  if (cfg.ssid[0]) {
    WiFi.begin(cfg.ssid, cfg.pass);
  } else {
    // Nothing to join, so offer the setup portal rather than sitting silent.
    // A device that cannot be configured without a cable is a device that
    // needs a laptop on site.
    portalBegin(cfg, devEui, []() { settingsSave(cfg); });
  }
  nodeWatchdogBegin(30);
  beep(2300, 60);
  pixel(0, 20, 0);
}

// ── Loop ────────────────────────────────────────────────────────────────────
void loop() {
  nodeWatchdogFeed();
  pollSerial();
  portalService();
  networkService();
  buzzer.service();       // before the annunciator, which queues into it
  ann.service();
  ann.mirrorBuzzer(buzzer);   // the centre LED moves with whatever is sounding

#if USE_SLIDER
  if (editing && program == P_THRESHOLD) {
    cfg.rhMax = Limits::RH_MIN +
                (sliderRaw() / 4095.0f) * (Limits::RH_MAX - Limits::RH_MIN);
  } else {
    program = sliderZone(P_COUNT);
  }
#endif

  // ── USER button: the whole control surface ────────────────────────────────
  // One button, three actions separated by press length:
  //
  //   tap      (< 1.2 s)  next program; while editing, +5 %RH and wrap
  //   hold     (1.2-5 s)  select -- on THRESHOLD, enter/leave edit and save
  //   hold     (>= 5 s)   raise the setup AP portal, from any program
  //
  // This replaces the slide potentiometer, which could not be read in any port
  // it was tried in. A device whose only selector is a module that may not be
  // plugged in is a device that cannot be operated.
  //
  // The band boundary gets its own beep. A press length the hand cannot feel
  // is a press length the hand gets wrong, and the only feedback otherwise is
  // the action itself -- by which point it is too late to hold on.
  static const unsigned long BTN_SELECT_MS = 1200;
  static const unsigned long BTN_PORTAL_MS = 5000;

  static bool lastBtn = HIGH;
  static unsigned long pressedAt = 0;
  static bool longFired = false;
  static bool selectArmed = false;
  bool btn = digitalRead(PIN_USER);

  if (lastBtn == HIGH && btn == LOW) {          // pressed
    noteActivity();
    // Any press takes the secret off the panel: it is displayed for someone
    // standing at the device, and that person pressing a button is the clearest
    // possible signal that they are done with it.
    if (secretShowing) { secretShowing = false; memset(secretShown, 0, sizeof(secretShown)); }
    pressedAt = millis();
    longFired = false;
    selectArmed = false;
  }

  unsigned long heldMs = (btn == LOW && pressedAt) ? millis() - pressedAt : 0;

  if (heldMs >= BTN_SELECT_MS && !selectArmed && !longFired) {
    selectArmed = true;
    beep(2400, 30);                             // "you are now in select"
  }

  if (btn == LOW && !longFired && pressedAt && heldMs >= BTN_PORTAL_MS) {
    longFired = true;
    holdPercent = 0;
    static const Note portalUp[3] = {{2600,120},{0,80},{3000,120}};
    buzzer.play(portalUp);
    portalBegin(cfg, devEui, []() { settingsSave(cfg); });
    program = P_SETTINGS;
  }

  if (btn == LOW && !longFired && pressedAt) {
    holdPercent = (int)(heldMs * 100 / BTN_PORTAL_MS);
    if (holdPercent > 100) holdPercent = 100;
  } else {
    holdPercent = 0;
  }

  if (lastBtn == LOW && btn == HIGH) {          // released
    unsigned long held = pressedAt ? millis() - pressedAt : 0;
    if (!longFired) {
      if (held >= BTN_SELECT_MS) {              // select
        if (program == P_THRESHOLD) {
          editing = !editing;
          if (!editing) { settingsSave(cfg); buzzer.play(Buzzer::MEL_SAVED); }
          else            beep(2400, 60);
        } else {
          beep(2050, 40);                       // nothing to select here
        }
      } else if (editing && program == P_THRESHOLD) {
        cfg.rhMax += Limits::RH_STEP;           // wrap rather than stick at the
        if (cfg.rhMax > Limits::RH_MAX)         // top, so one button can reach
          cfg.rhMax = Limits::RH_MIN;           // every value in one direction
        beep(2700, 25);
      } else {
        program = (program + 1) % P_COUNT;      // tap advances the menu
        beep(2600, 40);
      }
    }
    pressedAt = 0;
  }
  lastBtn = btn;

  if (millis() - lastRead > 2500) {             // a DHT11 cannot be rushed
    lastRead = millis();
    float t = dht.readTemperature(), h = dht.readHumidity();
    live.sensorOk = !isnan(t) && !isnan(h);
    if (live.sensorOk) {
      live.tempC = t; live.humidity = h;
      live.tMin = isnan(live.tMin) ? t : min(live.tMin, t);
      live.tMax = isnan(live.tMax) ? t : max(live.tMax, t);
    } else {
      live.tempC = NAN;        // a failed read must never look like a value
      live.humidity = NAN;
    }
    live.alarm = detectors.update(live, cfg);

    // The matrix says WHICH alarm, not merely that there is one. A single red
    // pixel for all three made "sensor dead" and "humidity high" look
    // identical across a room, and they need different responses.
    // WARN is played once, on the transition into an alarm, and interrupts the
    // per-alarm melody the annunciator queues on entry -- so you hear "there is
    // a problem", then on the repeat, which problem. A device that only plays
    // the specific contour makes people learn three sounds before they can
    // react to any of them.
    static Alarm lastAlarm = ALARM_NONE;
    if (live.alarm != ALARM_NONE) {
      pixel(60, 0, 0);
      switch (live.alarm) {
        case ALARM_HUMID:  ann.set(Annunciator::SIG_RH_HIGH);     break;
        case ALARM_AIRCON: ann.set(Annunciator::SIG_TEMP_RISE);   break;
        case ALARM_SENSOR: ann.set(Annunciator::SIG_SENSOR_DEAD); break;
        default: break;
      }
      if (lastAlarm == ALARM_NONE) buzzer.play(Buzzer::MEL_WARN);
      if (millis() - lastEvent > 60000) { lastEvent = millis(); publish(true); }
    } else {
      pixel(0, 15, 0);
      // Only stand down from an alarm pattern -- never interrupt the hourly
      // mark or the portal ring, which are not alarms and clear themselves.
      Annunciator::Signal cur = ann.current();
      if (cur == Annunciator::SIG_RH_HIGH || cur == Annunciator::SIG_TEMP_RISE ||
          cur == Annunciator::SIG_SENSOR_DEAD)
        ann.set(Annunciator::SIG_IDLE);
      if (lastAlarm != ALARM_NONE) buzzer.play(Buzzer::MEL_HEALTHY);
    }
    lastAlarm = live.alarm;
  }

  // Link state, announced on CHANGE only. A sound every time the loop noticed
  // the broker was down would be unbearable within a minute; a sound when it
  // goes down and another when it returns is information.
  //
  // Nothing is announced for the first 20 s: at boot the device is legitimately
  // offline while it associates, and reporting that as a failure trains people
  // to ignore the sound that matters.
  {
    static bool lastOnline = false, linkSettled = false;
    bool online = WiFi.status() == WL_CONNECTED && mqtt.connected();
    if (!linkSettled && millis() > 20000) { linkSettled = true; lastOnline = online; }
    if (linkSettled && online != lastOnline) {
      if (online) buzzer.play(Buzzer::MEL_HEALTHY);
      else        buzzer.play(Buzzer::MEL_NOCONN);
      lastOnline = online;
    }
  }

  if (sign.takeChanged()) {
    publishAvail();
    ann.scrollText(sign.name(), 0, 60, 30, 0, 1);
    buzzer.play(Buzzer::MEL_SAVED);
  }

  if (millis() - lastHist > 30000) { lastHist = millis(); detectors.pushHistory(live.tempC); }

  // Drain the queue while the broker will take it. A bounded batch per pass,
  // not the whole backlog: two months of records flushed in one loop would
  // block the button, the portal and the watchdog for minutes, and a broker
  // that accepted the first hundred and then dropped the connection would
  // leave the rest unsent anyway.
  if (mqtt.connected() && store.pending() > 0 &&
      millis() - lastFlush > 2000) {
    lastFlush = millis();
    uint32_t sent = store.flush(replayRecord, 20);
    if (sent) Serial.printf("#STORE,replayed %lu, %lu left\n",
                            (unsigned long)sent, (unsigned long)store.pending());
  }

  if (millis() - lastPublish > (unsigned long)cfg.publishS * 1000UL) {
    lastPublish = millis();
    publish(false);
  }

  // On the HOUR, not every 3600 s of uptime.
  //
  // The uptime version drifted to whatever minute the device last booted at,
  // so "every hour" meant 14:37, 15:37, 16:37 -- a proof-of-life nobody could
  // predict and therefore nobody could notice was missing. With a clock it
  // fires on the hour boundary, which is what anyone in the room expects.
  //
  // The uptime timer stays as the fallback for a device that has never reached
  // an NTP server: better an unpredictable hourly mark than none.
  bool hourStruck = false;
  if (timeIsValid()) {
    static int lastHourFired = -1;
    time_t now = time(nullptr);
    struct tm t;
    localtime_r(&now, &t);
    if (lastHourFired < 0) lastHourFired = t.tm_hour;   // do not fire on boot
    else if (t.tm_hour != lastHourFired) { lastHourFired = t.tm_hour; hourStruck = true; }
    lastHeartbeat = millis();          // keep the fallback from firing too
  } else if (millis() - lastHeartbeat > 3600000UL) {
    hourStruck = true;
  }

  if (hourStruck) {
    lastHeartbeat = millis();
    // Was three blocking beeps: 570 ms with the button unpolled, the portal
    // unserved and the watchdog unfed, every hour. The pattern now runs in the
    // background and stands itself down. An alarm outranks it.
    if (live.alarm == ALARM_NONE) ann.set(Annunciator::SIG_HOUR);
  }

  // The portal ring is driven from portal state rather than set once when the
  // portal opens, so it survives a signal that pre-empted it.
  if (portalRunning() && live.alarm == ALARM_NONE &&
      ann.current() == Annunciator::SIG_IDLE)
    ann.set(Annunciator::SIG_PORTAL);
  if (!portalRunning() && ann.current() == Annunciator::SIG_PORTAL)
    ann.set(Annunciator::SIG_IDLE);

  // Repaint only when something visible has changed.
  //
  // This used to fillScreen() and redraw every 400 ms, which blanks the whole
  // panel and back 2.5 times a second -- a visible blink. An ST7735 has no
  // double buffer, so every full repaint is seen. A cheap signature of what is
  // on screen decides whether a repaint is needed at all; the periodic path
  // remains only as a slow safety net.
  char sig[96];
  snprintf(sig, sizeof(sig), "%d|%d|%.1f|%.1f|%d|%.1f|%d|%d|%u",
           program, editing ? 1 : 0, live.tempC, live.humidity,
           (int)live.alarm, cfg.rhMax, WiFi.status() == WL_CONNECTED,
           mqtt.connected(), (unsigned)wakeCount);
  // Availability is part of what is on screen, so it belongs in the signature.
  // Without it a change arriving over MQTT would not repaint until the
  // temperature happened to move.
  strncat(sig, secretShowing ? "|S" : "|-", sizeof(sig) - strlen(sig) - 1);
  // The clock is drawn, so the minute belongs in the signature. Without it the
  // time would only change on the 5 s safety-net repaint and would visibly
  // lag, which on a clock is the one thing nobody forgives.
  char hmsig[8];
  clockHHMM(hmsig, sizeof(hmsig));
  strncat(sig, hmsig, sizeof(sig) - strlen(sig) - 1);
  strncat(sig, sign.name(), sizeof(sig) - strlen(sig) - 1);
  strncat(sig, sign.note(), sizeof(sig) - strlen(sig) - 1);
  // The hold progress bar and the portal state must repaint too, or the bar
  // would never appear and the screen would look frozen mid-press.
  char sig2[24];
  snprintf(sig2, sizeof(sig2), "|%d|%d", holdPercent / 5, portalRunning() ? 1 : 0);
  strncat(sig, sig2, sizeof(sig) - strlen(sig) - 1);
  static char lastSig[96] = "";
  bool changed = strcmp(sig, lastSig) != 0;

  if (changed || millis() - lastDraw > 5000) {
    strncpy(lastSig, sig, sizeof(lastSig) - 1);
    lastDraw = millis();
    screens.setProgram(program);
    // A secret on show outranks every program: it is on screen precisely
    // because somebody is standing there copying it down, and letting the
    // availability screen repaint over it would lose the only copy. An early
    // return here would also skip the sleep check below, so this is an else.
    if (secretShowing) {
      screens.secret("SIGN PASSWORD", secretShown);
    } else
    switch (program) {
      case P_AVAIL:     { char hm[8]; clockHHMM(hm, sizeof(hm));
                          screens.availability(sign.name(), sign.note(),
                                               mqtt.connected(), hm); } break;
      case P_MONITOR:   screens.monitor(live, cfg, mqtt.connected()); break;
      case P_THRESHOLD: screens.threshold(cfg, editing);              break;
      case P_SETTINGS:  screens.settings(cfg, portalRunning(), portalApSsid(),
                                         portalApPassword(), holdPercent, buzzer.muted()); break;
      case P_POWER:     screens.power(cfg, wakeCount);                break;
      default:          screens.about(devEui);                        break;
    }
  }

  // Battery mode: WiFi dominates the energy budget, so it connects only when
  // there is something to say. Timer wake only -- the onboard button is
  // GPIO 45 and only GPIO 0-21 are RTC-capable, so it cannot wake the board.
  // Sleep is a BATTERY behaviour. A device on mains has nothing to save by
  // sleeping, and a great deal to lose: the display goes dark, the web page
  // stops answering, and MQTT cannot reach it at all because deep sleep powers
  // down the radio. A mains-powered sign that disappears for five minutes at a
  // time is broken, not economical.
  //
  // The rule before this slept in either mode after idleS, which produced
  // exactly that on a window unit. And the rule before THAT was
  // `batteryMode && uptime > 20 s` -- not an idle rule at all, since on battery
  // it slept twenty seconds after boot whatever was happening.
  //
  // An alarm or an open setup portal holds it awake regardless: sleeping
  // through either is a fault, not a saving.
  bool idle = cfg.batteryMode && cfg.idleS > 0 &&
              (millis() - lastActivity) > (uint32_t)cfg.idleS * 1000UL;
  if (idle && live.alarm == ALARM_NONE && !portalRunning() && millis() > 20000) {
    publish(false);
    delay(200);
    Serial.printf("sleeping %u s\n", cfg.sleepS);
    Serial.flush();
    // Say goodnight, then let the melody actually finish. Deep sleep cuts the
    // pin dead mid-note otherwise, which sounds exactly like a crash -- the one
    // impression a scheduled sleep must not give.
    buzzer.play(Buzzer::MEL_SLEEP);
    uint32_t until = millis() + 700;
    while ((int32_t)(millis() - until) < 0) { buzzer.service(); nodeWatchdogFeed(); delay(5); }
    buzzer.silence();   // never enter deep sleep with the pin still driven
    esp_sleep_enable_timer_wakeup((uint64_t)cfg.sleepS * 1000000ULL);
    esp_deep_sleep_start();
  }
  delay(10);
}
