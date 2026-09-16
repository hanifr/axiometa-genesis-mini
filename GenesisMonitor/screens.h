#pragma once
// screens.h — what each program draws on the 160x80 panel.
//
// Drawing is kept apart from deciding: nothing here reads a sensor or changes
// a setting, so a display fault cannot alter behaviour and the detectors can
// be reasoned about without a screen attached.

#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <WiFi.h>
#include "globals.h"
#include "pins.h"

class Screens {
 public:
  explicit Screens(Adafruit_ST7735 &tft) : tft_(tft) {}

  // A secret, held on the panel to be written down. Same two-row treatment as
  // the AP password, for the same reason: one small line is unreadable at the
  // distance anyone actually stands from the device.
  void secret(const char *title, const char *value) {
    header(title, ST77XX_YELLOW);
    size_t n = strlen(value);
    tft_.setTextColor(ST77XX_WHITE);
    if (n && n <= 16) {
      size_t half = (n + 1) / 2;
      char a[9] = {0}, b[9] = {0};
      strncpy(a, value, half);
      strncpy(b, value + half, n - half);
      tft_.setTextSize(3);
      tft_.setCursor((160 - (int)strlen(a) * 18) / 2, 20); tft_.print(a);
      tft_.setCursor((160 - (int)strlen(b) * 18) / 2, 46); tft_.print(b);
    } else {
      tft_.setTextSize(2);
      tft_.setCursor(4, 34); tft_.print(value);
    }
    tft_.setTextSize(1);
    tft_.setTextColor(ST77XX_ORANGE);
    tft_.setCursor(4, 70); tft_.print("press USER when written down");
  }

  // Read through glass, from a corridor, by someone walking past. So: the
  // word itself at the largest size the panel will carry, in a colour that
  // carries the same meaning at a glance, and the note underneath. Nothing
  // else -- temperature belongs on MONITOR, which is for whoever owns the lab.
  void availability(const char *state, const char *note, bool mqttUp,
                    const char *clock) {
    uint16_t colour = !strcmp(state, "IN")   ? ST77XX_GREEN
                    : !strcmp(state, "OUT")  ? ST77XX_RED
                    : !strcmp(state, "BUSY") ? ST77XX_ORANGE
                                             : ST77XX_YELLOW;
    tft_.fillScreen(ST77XX_BLACK);
    current_ = P_AVAIL;

    // "IN CLASS" does not fit at size 3 on a 160 px panel, so it drops to 2
    // rather than being clipped mid-word.
    uint8_t size = strlen(state) > 4 ? 2 : 3;
    int w = strlen(state) * 6 * size;
    tft_.setTextSize(size);
    tft_.setTextColor(colour);
    tft_.setCursor((160 - w) / 2, size == 3 ? 14 : 20);
    tft_.print(state);

    // The clock sits directly under the state, at double height: it is the
    // second thing anyone walking past wants -- "is this current?" -- and a
    // sign showing a stale state is indistinguishable from a correct one
    // without it. Centred on its own width, like the state above.
    if (clock && clock[0]) {
      tft_.setTextSize(2);
      tft_.setTextColor(ST77XX_WHITE);
      int cw = strlen(clock) * 12;
      tft_.setCursor((160 - cw) / 2, size == 3 ? 42 : 44);
      tft_.print(clock);
    }

    tft_.setTextSize(1);
    tft_.setTextColor(ST77XX_WHITE);
    if (note && note[0]) {
      int nw = strlen(note) * 6;
      tft_.setCursor(nw < 160 ? (160 - nw) / 2 : 2, 62);
      tft_.print(note);
    }
    // A sign nobody can tell is stale is worse than no sign, so say whether
    // the device is still in touch with the broker that sets it.
    tft_.setTextColor(mqttUp ? ST77XX_GREEN : ST77XX_RED);
    tft_.setCursor(4, 72);
    tft_.print(mqttUp ? "live" : "offline");
    drawMenuBar(colour);
  }

  void monitor(const Readings &r, const Settings &cfg, bool mqttUp) {
    bool bad = r.alarm != ALARM_NONE;
    header(bad ? alarmText(r.alarm) : "MONITOR", bad ? ST77XX_RED : ST77XX_CYAN);

    tft_.setTextColor(ST77XX_WHITE);
    tft_.setTextSize(3);
    tft_.setCursor(4, 20);
    if (isnan(r.tempC)) tft_.print("--.-"); else tft_.print(r.tempC, 1);
    tft_.setTextSize(1); tft_.print(" C");

    tft_.setTextSize(2);
    tft_.setCursor(4, 50);
    if (isnan(r.humidity)) tft_.print("--"); else tft_.print(r.humidity, 0);
    tft_.setTextSize(1); tft_.print(" %RH");

    tft_.setTextColor(ST77XX_YELLOW);
    tft_.setCursor(96, 22); tft_.printf("max %.0f%%", cfg.rhMax);
    tft_.setCursor(96, 34);
    if (!isnan(r.tMin)) tft_.printf("%.0f-%.0fC", r.tMin, r.tMax);

    tft_.setTextColor(mqttUp ? ST77XX_GREEN : ST77XX_RED);
    tft_.setCursor(96, 52);
    tft_.print(WiFi.status() == WL_CONNECTED ? (mqttUp ? "mqtt ok" : "no broker")
                                             : "no wifi");
  }

  void threshold(const Settings &cfg, bool editing) {
    header(editing ? "THRESHOLD  *edit*" : "THRESHOLD", ST77XX_YELLOW);
    tft_.setTextColor(ST77XX_WHITE);
    tft_.setTextSize(2);
    tft_.setCursor(4, 22); tft_.printf("RH %.0f%%", cfg.rhMax);
    tft_.setTextSize(1);
    tft_.setCursor(4, 46); tft_.printf("cooling rise %.1f C", cfg.dTRise);
    tft_.setCursor(4, 58);
    // The label names the gesture, not the control. There is no slider.
    tft_.print(editing ? "tap +5%, hold to save" : "hold USER to edit");
  }

  // SETTINGS is where network and reporting are configured, and -- more
  // importantly -- where the device TELLS YOU how to configure it. A setup
  // route nobody can find is the same as not having one.
  void settings(const Settings &cfg, bool portalUp, const char *apSsid,
                const char *apPass, int holdPercent, bool buzzMuted) {
    header("SETTINGS", ST77XX_CYAN);
    tft_.setTextSize(1);

    if (portalUp) {
      // The password is the ONLY thing on this screen anyone needs to read, and
      // at size 1 a 12-character password is 72 of the panel's 160 pixels --
      // unreadable at the distance you actually stand from a mounted device.
      //
      // Split into two rows at size 3 it is triple the height AND easier to
      // transcribe, because six characters at a time is about what anyone can
      // hold while looking away. The SSID and the fixed portal address share
      // one small line above, which is all they need.
      size_t n = strlen(apPass);
      tft_.setTextSize(1);
      tft_.setTextColor(ST77XX_GREEN);
      tft_.setCursor(3, 16);
      tft_.printf("%s  192.168.4.1", apSsid);

      tft_.setTextColor(ST77XX_WHITE);
      if (n && n <= 16) {
        size_t half = (n + 1) / 2;            // odd lengths put the extra on top
        char a[9] = {0}, b[9] = {0};
        strncpy(a, apPass, half);
        strncpy(b, apPass + half, n - half);
        tft_.setTextSize(3);
        // 18 px per character at size 3; centre each row on its own width.
        tft_.setCursor((160 - (int)strlen(a) * 18) / 2, 28); tft_.print(a);
        tft_.setCursor((160 - (int)strlen(b) * 18) / 2, 54); tft_.print(b);
      } else {
        // Longer than the panel can carry at size 3: one row, still doubled.
        tft_.setTextSize(2);
        tft_.setCursor(4, 34);
        tft_.print(n ? apPass : "(not set)");
      }
      tft_.setTextSize(1);
      return;
    }

    tft_.setTextColor(ST77XX_WHITE);
    tft_.setCursor(4, 19);
    tft_.printf("net  %s", cfg.ssid[0] ? cfg.ssid : "(none set)");
    tft_.setCursor(4, 30);
    if (WiFi.status() == WL_CONNECTED)
      tft_.printf("ip   %s", WiFi.localIP().toString().c_str());
    else
      tft_.print("     not connected");
    tft_.setCursor(4, 41);
    tft_.printf("mqtt %s", cfg.mqttHost);
    // Mute is invisible until an alarm fails to sound, which is the worst
    // possible moment to learn about it. Say it on the settings screen.
    if (buzzMuted) {
      tft_.setTextColor(ST77XX_RED);
      tft_.setCursor(112, 41); tft_.print("MUTED");
      tft_.setTextColor(ST77XX_WHITE);
    }

    tft_.setTextColor(ST77XX_YELLOW);
    tft_.setCursor(4, 55);
    if (holdPercent > 0) {
      tft_.printf("hold... %d%%", holdPercent);
      tft_.fillRect(4, 66, holdPercent * 152 / 100, 4, ST77XX_YELLOW);
    } else {
      tft_.print("hold USER 5s for setup AP");
    }
  }

  void power(const Settings &cfg, uint32_t wakes) {
    header("POWER", ST77XX_CYAN);
    tft_.setTextColor(ST77XX_WHITE);
    tft_.setTextSize(1);
    tft_.setCursor(4, 20); tft_.printf("mode  %s", cfg.batteryMode ? "battery" : "mains");
    tft_.setCursor(4, 32); tft_.printf("sleep %u s", cfg.sleepS);
    // Read the pin, not the variant's getBatteryVoltage() -- that macro is
    // broken upstream, referencing an undefined VBAT_SENSE.
    int raw = analogRead(PIN_BAT_SENSE);
    tft_.setCursor(4, 44);
    tft_.printf("batt  %.2f V%s", raw / 4095.0 * 3.3 * 2.0, raw ? "" : " (none)");
    tft_.setCursor(4, 56); tft_.printf("wakes %u", (unsigned)wakes);

    // Say it here or it gets discovered in the field: GPIO 45 is not an RTC
    // pin, so the USER button cannot wake the board. Only the timer and RESET
    // can. A control that silently does nothing is worse than no control.
    tft_.setTextColor(ST77XX_YELLOW);
    tft_.setCursor(4, 68);
    if (!cfg.batteryMode)      tft_.print("mains: never sleeps");
    else if (cfg.idleS == 0)   tft_.print("sleep disabled");
    else                       tft_.print("asleep? RESET to wake");
  }

  void about(const char *eui) {
    header("ABOUT", ST77XX_CYAN);
    tft_.setTextColor(ST77XX_WHITE);
    tft_.setTextSize(1);
    tft_.setCursor(4, 20); tft_.print("Genesis Monitor");
    tft_.setCursor(4, 32); tft_.printf("eui  %s", eui);
    tft_.setCursor(4, 44); tft_.printf("up   %lu s", millis() / 1000);
    tft_.setCursor(4, 56); tft_.printf("heap %luk", (unsigned long)ESP.getFreeHeap()/1024);
  }

 private:
  void header(const char *title, uint16_t colour) {
    tft_.fillScreen(ST77XX_BLACK);
    tft_.setTextSize(1);
    tft_.setTextColor(colour);
    tft_.setCursor(3, 3);
    tft_.print(title);
    tft_.drawFastHLine(0, 13, 160, colour);
    drawMenuBar(colour);
  }

  // Five ticks along the top right, the current one filled. It mattered more
  // under the slider, where there was no way to tell how far to push to reach
  // a program you could not see. With a tap-to-advance button it still earns
  // its place: it shows how many taps remain to get back to MONITOR, and that
  // the menu wraps rather than stopping at the end.
  void drawMenuBar(uint16_t colour) {
    const int x0 = 160 - (P_COUNT * 9) - 4, y = 4, w = 6, h = 6;
    for (int i = 0; i < P_COUNT; i++) {
      int x = x0 + i * 9;
      if (i == current_) tft_.fillRect(x, y, w, h, colour);
      else               tft_.drawRect(x, y, w, h, 0x39E7);   // dim grey
    }
  }

 public:
  void setProgram(int p) { current_ = p; }
 private:
  int current_ = 0;
  Adafruit_ST7735 &tft_;
};
