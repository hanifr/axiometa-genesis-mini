#pragma once
// signage.h — what the device tells the corridor.
//
// This unit faces students through a lab window, so its primary job is no
// longer monitoring: it is a sign. Availability is therefore state the device
// owns and publishes, not a side effect of something else.
//
// Stored in its own NVS namespace rather than in Settings. Settings is one blob
// keyed by size, and adding a single uint16_t to it earlier today produced a
// struct that gained a field and no bytes -- byte-identical to the previous
// layout, which would have sent old records down the wrong migration branch.
// Signage state has no business taking that risk.
//
// RETAINED publishes, by request: the broker hands the last availability to
// anything that connects later, so a dashboard started on Monday shows the
// truth rather than nothing. The cost is that a stale value outlives a dead
// device -- which is why the device also publishes its uptime, so a consumer
// can tell a current "IN" from one left over from last week.

#include <Preferences.h>

enum Avail : uint8_t { AV_IN, AV_OUT, AV_BUSY, AV_CLASS, AV_COUNT };

static const char *AVAIL_NAME[AV_COUNT] = {"IN", "OUT", "BUSY", "IN CLASS"};

// Lower case, for matching an incoming payload. Longest distinctive prefix
// first: "in class" must be tested before "in", or it matches IN and the sign
// says the wrong thing.
static inline Avail availFromText(const char *t) {
  String w(t); w.trim(); w.toLowerCase();
  if (w.startsWith("in class") || w.startsWith("class") ||
      w.startsWith("teach")    || w.startsWith("lecture")) return AV_CLASS;
  if (w.startsWith("busy")     || w.startsWith("meeting")) return AV_BUSY;
  if (w.startsWith("out")      || w.startsWith("away"))    return AV_OUT;
  return AV_IN;
}

class Signage {
 public:
  void begin() {
    Preferences p;
    p.begin("signage", true);
    avail_ = (Avail)p.getUChar("avail", AV_IN);
    p.getString("note", note_, sizeof(note_));
    p.getString("pass", pass_, sizeof(pass_));
    p.end();
    if (avail_ >= AV_COUNT) avail_ = AV_IN;
  }

  void set(Avail a, const char *note) {
    avail_ = a;
    if (note) {
      strncpy(note_, note, sizeof(note_) - 1);
      note_[sizeof(note_) - 1] = '\0';
    }
    Preferences p;
    p.begin("signage", false);
    p.putUChar("avail", (uint8_t)avail_);
    p.putString("note", note_);
    p.end();
    changed_ = true;
  }

  // The sign's own write password, kept apart from the AP password.
  //
  // The AP password was the obvious token -- it already exists per device and
  // is printed on the SETTINGS screen. That screen faces the corridor. Raising
  // the setup portal to read the password displays it to exactly the audience
  // that must not be able to change the sign, so the two secrets are now
  // separate and this one is never drawn on the panel.
  //
  // Empty means fall back to the AP password, so an existing device keeps
  // working until somebody sets one.
  bool hasPassword() const { return pass_[0] != '\0'; }

  bool passwordMatches(const char *given, const char *apFallback) const {
    const char *want = pass_[0] ? pass_ : apFallback;
    if (!want || !want[0] || !given) return false;   // never authorise on empty
    return strcmp(given, want) == 0;
  }

  void setPassword(const char *pw) {
    strncpy(pass_, pw ? pw : "", sizeof(pass_) - 1);
    pass_[sizeof(pass_) - 1] = '\0';
    Preferences p;
    p.begin("signage", false);
    p.putString("pass", pass_);
    p.end();
  }

  uint8_t passwordLength() const { return (uint8_t)strlen(pass_); }

  Avail avail()      const { return avail_; }
  const char *name() const { return AVAIL_NAME[avail_]; }
  const char *note() const { return note_; }

  // True once after a change, so the caller can publish and repaint without
  // polling for a difference.
  bool takeChanged() { bool c = changed_; changed_ = false; return c; }

 private:
  Avail avail_ = AV_IN;
  char  note_[48] = "";
  char  pass_[33] = "";
  bool  changed_ = false;
};
