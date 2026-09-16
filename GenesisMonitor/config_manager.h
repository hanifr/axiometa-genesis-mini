#pragma once
// config_manager.h — the settings in NVS, and nowhere else.
//
// Stored as one blob keyed by size. A struct that grows would otherwise load
// garbage into the new fields; checking the length means an older record is
// ignored and the defaults stand, which is the safe direction to fail.

#include <Preferences.h>
#include "globals.h"

static Preferences prefs;

// The stored record is identified BY ITS SIZE, so two layouts that happen to
// be the same size would send an old record down the wrong migration branch
// and load garbage into the fields that follow. The compiler is the only thing
// that knows the real sizes on this target -- a reconstruction of these structs
// in a host-side test disagreed with it, which is exactly the kind of check
// that looks like evidence and is not.
static_assert(sizeof(Settings)   != sizeof(SettingsV1), "config layout collision: Settings/V1");
static_assert(sizeof(Settings)   != sizeof(SettingsV2), "config layout collision: Settings/V2");
static_assert(sizeof(Settings)   != sizeof(SettingsV3), "config layout collision: Settings/V3");
static_assert(sizeof(SettingsV1) != sizeof(SettingsV2), "config layout collision: V1/V2");
static_assert(sizeof(SettingsV1) != sizeof(SettingsV3), "config layout collision: V1/V3");
static_assert(sizeof(SettingsV2) != sizeof(SettingsV3), "config layout collision: V2/V3");
// Each layout must also be strictly larger than the one it replaced, or a
// field was dropped rather than added and the memcpy prefix trick is unsafe.
static_assert(sizeof(SettingsV1) < sizeof(SettingsV2), "V2 must extend V1");
static_assert(sizeof(SettingsV2) < sizeof(SettingsV3), "V3 must extend V2");
static_assert(sizeof(SettingsV3) < sizeof(SettingsV4), "V4 must extend V3");
static_assert(sizeof(SettingsV4) < sizeof(Settings),   "Settings must extend V4");
static_assert(sizeof(Settings)   != sizeof(SettingsV4), "config layout collision: Settings/V4");
static_assert(sizeof(SettingsV4) != sizeof(SettingsV3), "config layout collision: V4/V3");
static_assert(sizeof(SettingsV4) != sizeof(SettingsV2), "config layout collision: V4/V2");
static_assert(sizeof(SettingsV4) != sizeof(SettingsV1), "config layout collision: V4/V1");

static void settingsSave(const Settings &cfg) {
  prefs.begin("genesis", false);
  prefs.putBytes("cfg", &cfg, sizeof(cfg));
  prefs.end();
}

// Returns true when a stored record was used.
//
// A record written by an older build is MIGRATED, not discarded. Discarding is
// the safe direction for garbage, but a settings struct grows for ordinary
// reasons and the cost falls on whoever has to walk to the device and retype
// the WiFi password. Each old layout gets a branch here and is kept in
// globals.h; the migrated record is written straight back, so the next boot
// takes the fast path.
static bool settingsLoad(Settings &cfg) {
  prefs.begin("genesis", true);
  size_t n = prefs.getBytesLength("cfg");
  bool ok = (n == sizeof(cfg));
  bool migrated = false;

  if (ok) {
    prefs.getBytes("cfg", &cfg, sizeof(cfg));
    // Size alone is not identity. If the version does not match, the record is
    // some other layout that happens to be this size -- fall back to defaults
    // rather than load it and believe the result.
    if (cfg.cfgVersion != SETTINGS_VERSION) {
      Serial.printf("#CFG,stored version %lu is not %u; using defaults\n",
                    (unsigned long)cfg.cfgVersion, SETTINGS_VERSION);
      cfg = Settings{};
      ok = false;
    }
  } else if (n == sizeof(SettingsV4)) {
    SettingsV4 v4{};
    prefs.getBytes("cfg", &v4, sizeof(v4));
    cfg = Settings{};
    memcpy(&cfg, &v4, sizeof(v4));   // identical prefix
    cfg.ingestTopic[0] = '\0';      // new field keeps its default: disabled
    cfg.cfgVersion = SETTINGS_VERSION;
    ok = migrated = true;
  } else if (n == sizeof(SettingsV3)) {
    SettingsV3 v3{};
    prefs.getBytes("cfg", &v3, sizeof(v3));
    cfg = Settings{};
    memcpy(&cfg, &v3, sizeof(v3));   // identical prefix
    cfg.idleS = Settings{}.idleS;    // new fields keep their defaults
    cfg.cfgVersion = SETTINGS_VERSION;
    ok = migrated = true;
  } else if (n == sizeof(SettingsV2)) {
    SettingsV2 v2{};
    prefs.getBytes("cfg", &v2, sizeof(v2));
    cfg = Settings{};
    cfg.rhMax       = v2.rhMax;
    cfg.dTRise      = v2.dTRise;
    cfg.dTWindowS   = v2.dTWindowS;
    cfg.publishS    = v2.publishS;
    cfg.sleepS      = v2.sleepS;
    cfg.batteryMode = v2.batteryMode;
    memcpy(cfg.ssid,     v2.ssid,     sizeof(cfg.ssid));
    memcpy(cfg.pass,     v2.pass,     sizeof(cfg.pass));
    memcpy(cfg.apPass,   v2.apPass,   sizeof(cfg.apPass));
    memcpy(cfg.mqttHost, v2.mqttHost, sizeof(cfg.mqttHost));
    memcpy(cfg.pubTopic, v2.pubTopic, sizeof(cfg.pubTopic));
    cfg.mqttPort = v2.mqttPort;
    cfg.ssid[sizeof(cfg.ssid) - 1]         = '\0';
    cfg.pass[sizeof(cfg.pass) - 1]         = '\0';
    cfg.apPass[sizeof(cfg.apPass) - 1]     = '\0';
    cfg.mqttHost[sizeof(cfg.mqttHost) - 1] = '\0';
    cfg.pubTopic[sizeof(cfg.pubTopic) - 1] = '\0';
    ok = migrated = true;
  } else if (n == sizeof(SettingsV1)) {
    SettingsV1 v1{};
    prefs.getBytes("cfg", &v1, sizeof(v1));
    cfg = Settings{};                 // new fields keep their defaults
    cfg.rhMax       = v1.rhMax;
    cfg.dTRise      = v1.dTRise;
    cfg.dTWindowS   = v1.dTWindowS;
    cfg.publishS    = v1.publishS;
    cfg.sleepS      = v1.sleepS;
    cfg.batteryMode = v1.batteryMode;
    memcpy(cfg.ssid,   v1.ssid,   sizeof(cfg.ssid));
    memcpy(cfg.pass,   v1.pass,   sizeof(cfg.pass));
    memcpy(cfg.apPass, v1.apPass, sizeof(cfg.apPass));
    cfg.ssid[sizeof(cfg.ssid) - 1] = '\0';
    cfg.pass[sizeof(cfg.pass) - 1] = '\0';
    cfg.apPass[sizeof(cfg.apPass) - 1] = '\0';
    ok = migrated = true;
  }
  prefs.end();
  if (!ok) return false;

  // Clamp on the way IN as well as out. A value that became invalid because
  // the limits changed must not survive just because it is already stored.
  cfg.rhMax     = clampTo(cfg.rhMax,  Limits::RH_MIN, Limits::RH_MAX);
  cfg.dTRise    = clampTo(cfg.dTRise, Limits::DT_MIN, Limits::DT_MAX);
  cfg.publishS  = clampTo(cfg.publishS, Limits::PUB_MIN, Limits::PUB_MAX);
  cfg.sleepS    = clampTo(cfg.sleepS, Limits::SLEEP_MIN, Limits::SLEEP_MAX);
  if (cfg.mqttHost[0] == '\0') strncpy(cfg.mqttHost, MQTT_HOST_DEFAULT, sizeof(cfg.mqttHost) - 1);
  if (cfg.pubTopic[0] == '\0') strncpy(cfg.pubTopic, MQTT_TOPIC_DEFAULT, sizeof(cfg.pubTopic) - 1);
  if (cfg.mqttPort == 0)       cfg.mqttPort = MQTT_PORT_DEFAULT;
  // 0 is legal and means "never sleep" -- see #IDLE. Only non-zero values are
  // clamped to the usable range.
  if (cfg.idleS != 0)
    cfg.idleS = clampTo(cfg.idleS, Limits::IDLE_MIN, Limits::IDLE_MAX);

  if (migrated) {
    prefs.begin("genesis", false);
    prefs.putBytes("cfg", &cfg, sizeof(cfg));
    prefs.end();
    Serial.printf("#CFG,migrated a %u-byte record forward; settings kept\n",
                  (unsigned)n);
  }
  return true;
}

static void settingsClear() {
  prefs.begin("genesis", false);
  prefs.clear();
  prefs.end();
}
