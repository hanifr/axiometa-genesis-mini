#pragma once
// node_health.h — ESP32Eco
//
// Keeping a field node alive, rather than letting it stop quietly.
//
// Sibling sender sketches all did this on a failed radio init:
//
//     if (!LoRa.begin(BAND)) { display "NOT OK"; while (1); }
//
// which halts the board for good. There is no watchdog in that family, so a
// halt lasts until somebody walks to the cabinet and power-cycles it. On a gas
// sensor that is the worst possible failure: the node stops reporting, and at
// the receiver a halted node looks exactly like one that is out of range or
// simply has nothing to say.
//
// Two pieces here, both additive. A healthy node behaves exactly as before.
//
//   nodeInitRetry()  — call in a loop around a failed init. It reports on
//                      serial, waits with backoff, and restarts the board
//                      after a few attempts. A transient fault (a brown-out
//                      during SPI bring-up, a slow peripheral) then clears
//                      itself; a permanent one keeps saying so, which a halt
//                      never does.
//
//   nodeWatchdog*()  — an ESP task watchdog, so a node that hangs somewhere
//                      other than init also resets instead of going silent.
//                      Feed it once per loop.
//
// A restart loop is deliberately preferred to a halt. Both are bad, but a
// board that restarts keeps announcing the fault on serial and recovers the
// moment the cause goes away.

#include <Arduino.h>
#include <esp_task_wdt.h>

// Report a failed init, wait, and restart once attempts run out.
// Returns the attempt number, so a caller with a display can show it.
static inline int nodeInitRetry(const char *what, int attempt,
                                int maxAttempts = 6) {
  Serial.printf("[health] %s init failed (attempt %d of %d)\n",
                what, attempt, maxAttempts);
  Serial.flush();
  if (attempt >= maxAttempts) {
    Serial.printf("[health] %s still failing — restarting the board\n", what);
    Serial.flush();
    delay(200);
    ESP.restart();
  }
  // Back off a little each time: 500 ms, 1 s, 1.5 s ... so a peripheral that
  // is merely slow gets a chance before the board is bounced.
  delay(500UL * attempt);
  return attempt;
}

// Start the task watchdog. Call after the slow parts of setup(), or the
// watchdog fires during initialisation itself.
static inline void nodeWatchdogBegin(uint32_t seconds = 30) {
#if ESP_ARDUINO_VERSION_MAJOR >= 3
  // Core 3.x takes a config struct; the estate is on 2.0.17 today, and this
  // keeps the file compiling if that moves. See the LEDC guard in
  // firmware/vision-ml for the same pattern.
  esp_task_wdt_config_t cfg = {
    .timeout_ms = seconds * 1000,
    .idle_core_mask = 0,
    .trigger_panic = true,
  };
  esp_task_wdt_init(&cfg);
#else
  esp_task_wdt_init(seconds, true);
#endif
  esp_task_wdt_add(NULL);
}

// Feed it. Cheap; call once per loop().
static inline void nodeWatchdogFeed() {
  esp_task_wdt_reset();
}
