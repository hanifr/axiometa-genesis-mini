#pragma once
// detectors.h — deciding when something has actually changed.
//
// Three detectors, and the reasoning behind each is the point of this file.
//
// COOLING LOST is a RATE, not a level. "Too warm" fires every afternoon and
// tells you nothing; a sustained rise over ten minutes is the signature of
// cooling stopping. A door opening also raises the temperature, but it spikes
// and recovers inside the window, so it does not qualify.
//
// SENSOR DEAD exists because no threshold can catch it. A dead sensor reports
// a legal value. In this estate the industrial gas nodes return 0 on a failed Modbus
// read, and 0 %LEL reads as SAFE -- the reassuring number is the failure. Here
// a failed read stores NaN and repeated identical readings raise the alarm.
//
// HUMIDITY HIGH is the ordinary one, and is a level.

#include <math.h>
#include "globals.h"

#define HIST_POINTS 20          // 20 samples at 30 s = the 10 minute window

class Detectors {
 public:
  void reset() { count_ = 0; noAnswer_ = 0; frozen_ = 0; lastT_ = lastH_ = NAN; }

  // Call on every sensor tick.
  Alarm update(const Readings &r, const Settings &cfg) {
    // NOT ANSWERING is a dead sensor. Repeated identical numbers are not:
    // a DHT11 has 1 degC and 1 %RH resolution, so in a stable room the same
    // integers repeat for minutes on end. An earlier version raised the alarm
    // after twenty identical temperatures -- fifty seconds -- and fired within
    // a minute of being switched on, on a perfectly healthy sensor.
    //
    // So silence counts quickly, and frozen values count only when BOTH
    // channels freeze together for a long time. Humidity wanders even when
    // temperature is quantised flat, so both stuck at once is genuinely odd.
    if (isnan(r.tempC) || isnan(r.humidity)) {
      noAnswer_++;
      frozen_ = 0;
    } else {
      noAnswer_ = 0;
      bool sameT = !isnan(lastT_) && fabsf(r.tempC - lastT_) < 0.001f;
      bool sameH = !isnan(lastH_) && fabsf(r.humidity - lastH_) < 0.001f;
      frozen_ = (sameT && sameH) ? frozen_ + 1 : 0;
      lastT_ = r.tempC;
      lastH_ = r.humidity;
    }
    if (noAnswer_ >= NO_ANSWER_LIMIT || frozen_ >= FROZEN_LIMIT) return ALARM_SENSOR;

    if (!isnan(r.humidity) && r.humidity >= cfg.rhMax) return ALARM_HUMID;

    if (count_ >= HIST_POINTS && !isnan(r.tempC) &&
        (r.tempC - hist_[0]) >= cfg.dTRise) return ALARM_AIRCON;

    return ALARM_NONE;
  }

  // Call every 30 s, not every tick: the window is defined in samples.
  void pushHistory(float tempC) {
    if (isnan(tempC)) return;
    if (count_ < HIST_POINTS) {
      hist_[count_++] = tempC;
    } else {
      memmove(hist_, hist_ + 1, sizeof(float) * (HIST_POINTS - 1));
      hist_[HIST_POINTS - 1] = tempC;
    }
  }

  int   historyCount() const { return count_; }
  int   noAnswerRun() const { return noAnswer_; }
  int   frozenRun() const { return frozen_; }
  float oldest() const { return count_ ? hist_[0] : NAN; }

 private:
  // At one tick every 2.5 s: six missed reads is about fifteen seconds, and
  // 240 frozen ticks is ten minutes of BOTH channels not moving at all.
  static const int NO_ANSWER_LIMIT = 6;
  static const int FROZEN_LIMIT    = 240;
  float hist_[HIST_POINTS];
  int   count_ = 0;
  int   noAnswer_ = 0;
  int   frozen_ = 0;
  float lastT_ = NAN, lastH_ = NAN;
};
