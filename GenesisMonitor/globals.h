#pragma once
// globals.h — what the device is, what it may be set to, and the limits.
//
// Everything tunable lives in one struct so config_manager.h can persist it as
// a single blob, and so there is one place to look when a value seems wrong.

#include <Arduino.h>

// The broker the device ships pointing at, and the greeting it scrolls. These
// are DEFAULTS now, not the values in use -- the broker is editable and lives
// in Settings, because a device that can only be repointed by reflashing is a
// device that gets reflashed on site.
#define MQTT_HOST_DEFAULT  "broker.example.com"
#define MQTT_PORT_DEFAULT  1883

// {eui} is replaced with this device's identifier when publishing, so one
// topic template works across a fleet without editing each unit. Events are
// published to this topic with "/event" appended.
#define MQTT_TOPIC_DEFAULT "sensors/{eui}/data"

// Event, availability and command topics are SIBLINGS of the data topic, built
// by siblingTopic() in the sketch: the configured topic names the data stream,
// so the others sit beside it rather than underneath it. One thing to
// configure, and a shape a dashboard can guess.

#define GREETING_DEFAULT   "GENESIS MINI READY"

// The zone used for DISPLAY only. Timestamps that travel to a broker or a
// database stay UTC epoch -- rendering them locally is the display's job, and
// getting that backwards is how two readings an hour apart come to look
// simultaneous twice a year. POSIX inverts the sign, so UTC+8 is "-8".
// Malaysia has no daylight saving, hence no DST rule.
#define DISPLAY_TZ         "MYT-8"

// mDNS name, so the sign is reachable without knowing a DHCP address:
// http://genesis.local. Advisory only -- plenty of networks block multicast,
// which is why the IP is printed at boot as well.
#define MDNS_NAME_DEFAULT  "genesis"

#define SETTINGS_VERSION 5u

// ── Settings, persisted whole ───────────────────────────────────────────────
//
// ⚠ ADDING A FIELD CHANGES sizeof AND INVALIDATES EVERY STORED RECORD, which
// means the WiFi credentials someone typed on site are gone at the next flash.
// If you add one, add a migration in config_manager.h and keep the previous
// layout below, exactly as SettingsV1 is kept.
struct Settings {
  float    rhMax       = 70.0;   // %RH — alarm above this
  float    dTRise      = 2.0;    // degC rise that counts as cooling lost
  uint16_t dTWindowS   = 600;    // seconds the rise must persist
  uint16_t publishS    = 60;     // MQTT publish interval
  uint16_t sleepS      = 300;    // battery mode wake interval
  bool     batteryMode = false;
  char     ssid[33]    = "";
  char     pass[65]    = "";
  char     apPass[64]  = "";     // this device's own AP password, generated once
  char     mqttHost[64] = MQTT_HOST_DEFAULT;
  uint16_t mqttPort     = MQTT_PORT_DEFAULT;
  char     pubTopic[72] = MQTT_TOPIC_DEFAULT;
  char     mqttUser[33] = "";    // blank = connect anonymously
  char     mqttPass[65] = "";
  uint16_t idleS        = 300;   // sleep after this long with nothing happening
  // Where an ingest back-end listens. EMPTY = ingest disabled, which is the
  // default: a device does not start publishing into somebody else's
  // production namespace because its firmware was updated.
  char     ingestTopic[72] = "";
  // Identifying a layout purely BY SIZE is fragile: adding idleS as a uint16_t
  // consumed tail padding and produced a struct one field larger and zero
  // bytes bigger, which a static_assert caught and a host-side size test did
  // not. This field makes the current layout self-identifying, and bumping it
  // is now part of adding a field.
  uint32_t cfgVersion   = SETTINGS_VERSION;
};

// The layout before the Back-end ingest topic was added (2026-09-16).
struct SettingsV4 {
  float    rhMax;
  float    dTRise;
  uint16_t dTWindowS;
  uint16_t publishS;
  uint16_t sleepS;
  bool     batteryMode;
  char     ssid[33];
  char     pass[65];
  char     apPass[64];
  char     mqttHost[64];
  uint16_t mqttPort;
  char     pubTopic[72];
  char     mqttUser[33];
  char     mqttPass[65];
  uint16_t idleS;
  uint32_t cfgVersion;
};

// The layout before the idle timeout was added (2026-09-15).
struct SettingsV3 {
  float    rhMax;
  float    dTRise;
  uint16_t dTWindowS;
  uint16_t publishS;
  uint16_t sleepS;
  bool     batteryMode;
  char     ssid[33];
  char     pass[65];
  char     apPass[64];
  char     mqttHost[64];
  uint16_t mqttPort;
  char     pubTopic[72];
  char     mqttUser[33];
  char     mqttPass[65];
};

// The layout before the broker credentials were added (2026-09-15).
struct SettingsV2 {
  float    rhMax;
  float    dTRise;
  uint16_t dTWindowS;
  uint16_t publishS;
  uint16_t sleepS;
  bool     batteryMode;
  char     ssid[33];
  char     pass[65];
  char     apPass[64];
  char     mqttHost[64];
  uint16_t mqttPort;
  char     pubTopic[72];
};

// The layout before the MQTT fields were added (2026-09-15). Kept so a record
// written by that build can be read and carried forward instead of discarded;
// deleting this silently wipes the network settings of every unit in the
// field on their next update.
struct SettingsV1 {
  float    rhMax;
  float    dTRise;
  uint16_t dTWindowS;
  uint16_t publishS;
  uint16_t sleepS;
  bool     batteryMode;
  char     ssid[33];
  char     pass[65];
  char     apPass[64];
};

// Ceilings, applied on every write. A setting that can be typed can be
// mistyped, and a humidity alarm at 300 %RH never fires -- which is worse than
// refusing the value, because it looks configured.
namespace Limits {
  static const float    RH_MIN = 20.0,  RH_MAX = 95.0;
  // One tap of the USER button moves the threshold by this much. Five
  // percent over a 20-95 range is 15 taps end to end -- coarse enough to
  // cross the range without a sore thumb, fine enough to be useful.
  static const float    RH_STEP = 5.0;
  static const float    DT_MIN = 0.5,   DT_MAX = 10.0;
  static const uint16_t PUB_MIN = 10,   PUB_MAX = 3600;
  static const uint16_t SLEEP_MIN = 30, SLEEP_MAX = 3600;
  // Never below a minute: anything shorter and the device sleeps while someone
  // is still reading the screen it just drew.
  static const uint16_t IDLE_MIN = 60, IDLE_MAX = 7200;
}

template <typename T>
static inline T clampTo(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ── Programs ────────────────────────────────────────────────────────────────
// The stock Kit Demo is a menu of seven, four of them games. Same shape, work
// instead of toys.
// P_AVAIL is first because this unit faces a corridor: the availability sign
// is what should be on screen when it boots and what it returns to. The menu
// order is also the tap order, so the sign is never more than a wrap away.
enum Program { P_AVAIL, P_MONITOR, P_THRESHOLD, P_SETTINGS, P_POWER, P_ABOUT,
               P_COUNT };
static const char *PROGRAM_NAME[P_COUNT] = {
  "AVAILABLE", "MONITOR", "THRESHOLD", "SETTINGS", "POWER", "ABOUT"
};

// ── Alarms ──────────────────────────────────────────────────────────────────
enum Alarm { ALARM_NONE, ALARM_HUMID, ALARM_AIRCON, ALARM_SENSOR };

// PubSubClient's state codes, in words. A failed MQTT connection was silent:
// the display said "no broker" whether the credentials were wrong, the port
// was shut, or DNS never resolved -- three problems with three different
// fixes, presented identically.
static inline const char *mqttStateText(int st) {
  switch (st) {
    case -4: return "timeout";
    case -3: return "connection lost";
    case -2: return "connect failed (host/port unreachable)";
    case -1: return "disconnected";
    case  0: return "connected";
    case  1: return "bad protocol";
    case  2: return "bad client id";
    case  3: return "broker unavailable";
    case  4: return "BAD CREDENTIALS";
    case  5: return "not authorised";
    default: return "unknown";
  }
}

static inline const char *alarmText(Alarm a) {
  switch (a) {
    case ALARM_HUMID:  return "HUMIDITY HIGH";
    case ALARM_AIRCON: return "COOLING LOST";
    case ALARM_SENSOR: return "SENSOR DEAD";
    default:           return "none";
  }
}

// ── Live readings, shared across the modules ────────────────────────────────
struct Readings {
  float tempC = NAN, humidity = NAN;
  float tMin  = NAN, tMax = NAN;
  bool  sensorOk = false;
  Alarm alarm = ALARM_NONE;
};
