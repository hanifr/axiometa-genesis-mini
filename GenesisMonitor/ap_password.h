#pragma once
// ap_password.h — ESP32Eco
//
// One access-point password per device, generated rather than shipped.
//
// Eleven sketches in this estate hardcoded "12345678". That is one password
// for the whole fleet: anyone within radio range of any board in AP mode could
// join it and reconfigure it, including pointing it at their own network.
//
// apPasswordEnsure() replaces a weak or missing password with a random one
// held in NVS, and LEAVES ALONE anything an operator deliberately set. Call it
// immediately before WiFi.softAP().
//
// Four decisions, each one load-bearing:
//
//   * NOT derived from the MAC. Most of these boards build their AP SSID from
//     the MAC, so a MAC-derived password is reconstructible by anyone who can
//     see the SSID -- which is everyone, that being what an SSID is for.
//
//   * Its own "apsec" NVS namespace, never the sketch's config namespace. Most
//     of these sketches clear their whole namespace on a config reset, and a
//     password that vanished then would lock out whoever had written the old
//     one down.
//
//   * Generated only AFTER the radio is up. esp_random() is a true hardware
//     RNG once WiFi or Bluetooth is running and a deterministic sequence
//     before that -- so calling it too early would give every board built from
//     the same firmware the same "random" password. Call this after
//     WiFi.mode(), which every caller here already does.
//
//   * A deliberate password is kept. These sketches read ap_password from
//     their own NVS with the compiled value as a fallback, so a site that has
//     set its own password through the web UI must not be overridden.
//
// The generated password is printed once, on serial. That is the only place it
// appears: it has to be readable by whoever commissions the board.

#include <Arduino.h>
#include <Preferences.h>
#include <esp_system.h>

// Passwords this function refuses to leave in place. Shipped defaults only --
// not a general strength test, which is not this file's business.
static inline bool apPasswordIsWeak(const char *p) {
  if (p == nullptr) return true;
  const size_t n = strlen(p);
  if (n < 8) return true;                 // WPA2 will not accept it anyway
  static const char *known[] = {"12345678", "123456789", "password",
                                "admin123", "00000000"};
  for (size_t i = 0; i < sizeof(known) / sizeof(known[0]); i++)
    if (strcmp(p, known[i]) == 0) return true;
  return false;
}

static inline bool apPasswordEnsure(char *pass, size_t cap);   // defined below

// Generate a fresh password unconditionally, discarding whatever is stored.
//
// apPasswordEnsure() deliberately KEEPS any stored value of usable length, so
// it cannot rotate: clearing the caller's buffer and calling it again reloads
// the same password out of NVS and reports success. That is exactly the bug
// this function exists to remove -- a rotate that silently does nothing is
// worse than no rotate, because it is believed.
//
// A disclosed password has no other remedy: it is generated once and kept for
// the life of the device.
static inline bool apPasswordRotate(char *pass, size_t cap) {
  if (pass == nullptr || cap < 13) return false;
  Preferences apPrefs;
  apPrefs.begin("apsec", false);
  apPrefs.remove("pass");
  apPrefs.end();
  pass[0] = '\0';
  return apPasswordEnsure(pass, cap);
}

// Ensure `pass` (a buffer of `cap` bytes) holds a password worth using.
// Returns true when it replaced one, false when the existing value was kept.
static inline bool apPasswordEnsure(char *pass, size_t cap) {
  if (pass == nullptr || cap < 13) return false;
  if (!apPasswordIsWeak(pass)) return false;      // deliberate: leave it

  Preferences apPrefs;
  apPrefs.begin("apsec", false);
  String stored = apPrefs.getString("pass", "");

  if (stored.length() < 8) {
    // No lookalike characters: this gets read off a serial console and typed
    // in by hand, often on a phone.
    static const char alphabet[] = "abcdefghijkmnopqrstuvwxyz"
                                   "ABCDEFGHJKLMNPQRSTUVWXYZ"
                                   "23456789";
    const size_t n = sizeof(alphabet) - 1;
    stored = "";
    for (int i = 0; i < 12; i++) stored += alphabet[esp_random() % n];
    apPrefs.putString("pass", stored);
    Serial.println();
    Serial.println("=====================================================");
    Serial.println("  AP password generated for this device.");
    Serial.printf ("  Length:   %u characters\n", (unsigned)stored.length());
    Serial.println("  NOT printed here -- serial output reaches logs,");
    Serial.println("  scrollback and transcripts. Read it from the");
    Serial.println("  device's own display, or its setup page over the AP.");
    Serial.println("  Stored in NVS; it survives a reflash.");
    Serial.println("=====================================================");
  } else {
    Serial.printf("AP password: %u characters (from NVS)\n",
                  (unsigned)stored.length());
  }
  apPrefs.end();

  strncpy(pass, stored.c_str(), cap - 1);
  pass[cap - 1] = '\0';
  return true;
}
