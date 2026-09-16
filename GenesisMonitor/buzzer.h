#pragma once
// buzzer.h — the only thing that touches the buzzer pin.
//
// It previously had TWO owners: beep() in the sketch on LEDC channel 0 and the
// annunciator on channel 1, each calling ledcAttachPin/ledcDetachPin on the
// same GPIO. ledcDetachPin detaches the pin from WHATEVER channel holds it, so
// a UI click could silence an in-progress alarm tone, and the annunciator then
// believed it was still sounding. That was patched by re-attaching on every
// step; this replaces the patch with single ownership, which is the actual fix.
//
// Everything is non-blocking. A melody is a list of notes advanced by
// service(); nothing here calls delay().
//
// Mute and volume live in their own NVS namespace rather than in Settings.
// Settings is stored as one blob keyed by sizeof, so adding a field there
// silently invalidates a saved configuration -- including the WiFi credentials
// -- on the next flash. The same reasoning the AP password helper uses.

#include <Preferences.h>
#include "pins.h"

// hz 0 = a rest of ms. vol 0 = "use the configured volume"; 1-10 overrides it
// for this note only, which is what lets a chime decay instead of stopping
// dead. A square wave that just cuts off sounds like a fault; a tail sounds
// deliberate.
struct Note { uint16_t hz; uint16_t ms; uint8_t vol; };

// ── The startup chime ──────────────────────────────────────────────────────
// Apple's chime is a G-flat major chord, and their trademark registration
// specifies it with concert A at 432.4 Hz rather than 440 -- so it sits very
// slightly FLAT of standard tuning. Jim Reekes played it on a Korg Wavestation
// as a chord voiced with both hands stretched as wide as possible, third on
// top, and its character is a long reverberant decay of two seconds or more.
//
// Those pitches taken up to octave 7, where this buzzer is loud, with the
// third (B-flat) on top as in the original voicing:
//
//   Db7 = 432.4 * 2^(4/12)  * 4 = 2179 Hz
//   Gb7 = 432.4 * 2^(-3/12) * 8 = 2909 Hz
//   Bb7 = 432.4 * 2^(1/12)  * 8 = 3665 Hz
//
// This is a reconstruction of the published chord and tuning on a one-voice
// buzzer, not Apple's recording, which is a sampled instrument with reverb.
static const uint16_t CHIME_GB_MAJOR[3] = {2179, 2909, 3665};
// 4200 ms, chosen by ear against 3200 and 5000 played back to back. Long
// enough for the tail to be a tail -- the ring is most of what people
// recognise about this chime, and the earlier 2400 ms version spent its last
// third below the audible floor anyway.
static const uint16_t CHIME_MS = 4200;


class Buzzer {
 public:
  // The pin is STORED, not compiled in. Which of a port's three lines a module
  // actually drives is not predictable here -- on this board the DHT11 uses
  // IO1, the matrix uses IO1 and the buzzer had been assumed to be IO0 without
  // anyone ever confirming it by ear. #BZSCAN finds it, #BZPIN saves it.
  void begin() {
    Preferences p;
    p.begin("buzzcfg", true);
    muted_  = p.getBool("mute", false);
    volume_ = p.getUChar("vol", 7);
    pin_    = p.getUChar("pin", BUZZER_PIN);
    p.end();
    if (volume_ > 10) volume_ = 10;
    attach();
  }

  void setPin(uint8_t gpio) {
    silence();
    ledcDetachPin(pin_);
    pin_ = gpio;
    Preferences p; p.begin("buzzcfg", false); p.putUChar("pin", gpio); p.end();
    attach();
  }
  uint8_t pin() const { return pin_; }

  // One note. Replaces the old blocking beep(hz, ms) at every call site: it
  // returns immediately and the note ends on its own.
  void note(uint16_t hz, uint16_t ms) {
    one_.hz = hz; one_.ms = ms; one_.vol = 0;
    play(&one_, 1);
  }

  // Prefer this overload: the length comes from the array, so a melody can be
  // lengthened or shortened without hunting down every call site. Shortening
  // MEL_STARTUP from 8 notes to 6 while seven call sites still passed a
  // hand-written count would have read past the end of the array and played
  // whatever followed it in flash.
  template <size_t N>
  void play(const Note (&seq)[N]) { play(seq, (uint8_t)N); }

  // ── Struck chord ─────────────────────────────────────────────────────────
  // Strum a chord and decay it, COMPUTED rather than tabulated. A hand-written
  // note table cannot express a smooth two-second fade: the previous attempt
  // was 36 entries stepping the level 10..1, and it sounded like 36 entries
  // stepping the level 10..1.
  //
  // One voice cycling three pitches every ~11 ms is the closest a piezo gets
  // to a chord. It will never fuse completely -- fusion needs a cycle rate
  // above roughly 100 Hz, which leaves too few wave cycles per note to carry a
  // pitch at all -- so what this actually produces is a fast tremolo chord.
  // That is the honest ceiling for this part.
  void chord(const uint16_t *freqs, uint8_t n, uint16_t totalMs,
             uint16_t stepMs = 11) {
    if (!n) return;
    chordFreqs_ = freqs; chordN_ = n;
    chordStepMs_ = stepMs ? stepMs : 11;
    chordTotal_ = totalMs;
    chordStart_ = millis();
    chordIdx_ = 0;
    seq_ = nullptr; count_ = 0; idx_ = 0; inGap_ = false;
    mode_ = CHORD;
    nextAt_ = chordStart_;
    service();
  }

  void play(const Note *seq, uint8_t count) {
    seq_ = seq; count_ = count; idx_ = 0; nextAt_ = millis();
    mode_ = SEQ; inGap_ = false;
    service();
  }

  void silence() {
    seq_ = nullptr; count_ = 0; idx_ = 0; inGap_ = false; mode_ = SEQ;
    curHz_ = 0; curLvl_ = 0;
    ledcWrite(CH, 0);
  }

  bool busy() const { return seq_ != nullptr || mode_ == CHORD; }

  // What is sounding right now, for anything that wants to move in time with
  // it. Zero hz means a rest -- which matters: the gaps are as much a part of
  // a rhythm as the notes, so a light that ignores them just stays on.
  bool     sounding() const { return curHz_ != 0; }
  uint16_t hz()       const { return curHz_; }
  uint8_t  level()    const { return curLvl_; }

  void service() {
    uint32_t now = millis();
    if (mode_ == CHORD) { serviceChord(now); return; }
    if (!seq_) return;
    if ((int32_t)(now - nextAt_) < 0) return;

    // Every note ends a few ms early, in silence, before the next begins.
    // Without it one square wave runs straight into the next at a different
    // frequency and the discontinuity is heard as a click on every note --
    // which is most of what made these sound cheap.
    if (inGap_) {
      ledcWrite(CH, 0);
      curHz_ = 0;
      inGap_ = false;
      idx_++;
      nextAt_ = now + GAP_MS;
      return;
    }
    if (idx_ >= count_) { silence(); return; }

    const Note &n = seq_[idx_];

    // Notes of 30 ms or less run together with no gap. That is what lets a
    // chord be strummed: a piezo has one voice, so the only way to suggest
    // three notes at once is to cycle them faster than the ear separates
    // them -- and an 8 ms silence between each would chop that back into
    // three notes again. Longer notes are still articulated.
    bool legato = n.ms <= LEGATO_MS;
    uint16_t dur = (!legato && n.ms > (GAP_MS + 10)) ? (uint16_t)(n.ms - GAP_MS) : n.ms;
    if (n.hz && !muted_) {
      ledcWriteTone(CH, n.hz);
      ledcWrite(CH, n.vol ? (uint16_t)(n.vol > 10 ? 10 : n.vol) * 51 : duty());
    } else {
      ledcWrite(CH, 0);                 // a rest, or muted: still burn the time
    }
    // Reported even when muted, so the light still shows the rhythm of a
    // melody somebody has silenced. A muted alarm should not be an invisible
    // one.
    curHz_  = n.hz;
    curLvl_ = n.vol ? (n.vol > 10 ? 10 : n.vol) : volume_;
    if (legato) idx_++; else inGap_ = true;
    nextAt_ = now + dur;
  }

  // Muting stops sound immediately rather than at the end of the current note.
  // A buzzer that keeps going for another two seconds after you mute it is a
  // buzzer someone unplugs.
  void setMute(bool m) {
    muted_ = m;
    if (m) ledcWrite(CH, 0);
    Preferences p; p.begin("buzzcfg", false); p.putBool("mute", m); p.end();
  }
  bool muted() const { return muted_; }

  // 0..10. A piezo is loudest near 50% duty and quieter either side, so this
  // maps onto 0..50% only -- going past it makes no more noise, just a dirtier
  // waveform.
  void setVolume(uint8_t v) {
    volume_ = v > 10 ? 10 : v;
    Preferences p; p.begin("buzzcfg", false); p.putUChar("vol", volume_); p.end();
  }
  uint8_t volume() const { return volume_; }

  // ── Melodies ──────────────────────────────────────────────────────────────
  // Each alarm gets a CONTOUR, not just a pitch: rising means something is
  // climbing, falling means something stopped, and a flat repeated pair means
  // attention. Across a room the contour is what people recognise -- a single
  // tone tells you only that the box is unhappy.
  static const Note MEL_WELCOME[4];
  static const Note MEL_RH[4];         // urgent, flat pair repeated
  static const Note MEL_TEMP[3];       // rising: temperature is climbing
  static const Note MEL_DEAD[3];       // falling: something stopped
  static const Note MEL_HOUR[3];       // bright, brief
  static const Note MEL_SAVED[2];
  static const Note MEL_CLICK[1];
  // The boot chime is a computed chord, not a note table: see chord().
  static void startupChime(Buzzer &bz) { bz.chord(CHIME_GB_MAJOR, 3, CHIME_MS); }
  static const Note MEL_SLEEP[5];     // going to deep sleep
  static const Note MEL_WARN[7];      // something is wrong -- said once, first
  static const Note MEL_HEALTHY[3];   // online and nothing wrong
  static const Note MEL_NOCONN[5];    // the link is down

 private:
  // Level falls as the SQUARE of the remaining time -- a struck string loses
  // most of its energy early and then rings on quietly. A linear fade sounds
  // like a volume knob being turned down, which is the tell of a synthesised
  // chime rather than a struck one.
  void serviceChord(uint32_t now) {
    uint32_t elapsed = now - chordStart_;
    if (elapsed >= chordTotal_) { silence(); return; }
    if ((int32_t)(now - nextAt_) < 0) return;

    // Two-stage decay. The square law used before reached level 1 by the time
    // a third of the duration remained -- and level 1 is 5% duty, which on this
    // buzzer is inaudible, so the entire tail was silence. It measured as a
    // long chime and sounded like a short one.
    //
    // Now: 10 down to 5 over the first quarter, which is the strike losing its
    // energy, then 5 down to 1 across the remaining three quarters, which is
    // the ring. The tail stays above the audible floor nearly to the end.
    uint32_t remain = chordTotal_ - elapsed;
    uint32_t r = (remain * 1000UL) / chordTotal_;       // 1000 at the strike
    uint32_t lvl = (r > 750) ? 10 - ((1000 - r) * 5) / 250
                             :  5 - (( 750 - r) * 4) / 750;
    if (lvl > 10) lvl = 10;
    if (lvl < 1)  lvl = 1;

    if (!muted_) {
      ledcWriteTone(CH, chordFreqs_[chordIdx_ % chordN_]);
      ledcWrite(CH, (uint16_t)lvl * 51);
    } else {
      ledcWrite(CH, 0);
    }
    curHz_  = chordFreqs_[chordIdx_ % chordN_];
    curLvl_ = (uint8_t)lvl;
    chordIdx_++;
    nextAt_ = now + chordStepMs_;
  }

  void attach() {
    ledcSetup(CH, 2000, 10);
    ledcAttachPin(pin_, CH);
    ledcWrite(CH, 0);
  }

  static const uint8_t CH = 0;         // the only channel used on this pin
  static const uint16_t GAP_MS = 8;     // articulation between notes
  static const uint16_t LEGATO_MS = 30; // at or below this, no gap
  uint16_t duty() const { return (uint16_t)volume_ * 51; }  // 0..510 of 1023

  const Note *seq_ = nullptr;
  uint8_t  count_ = 0, idx_ = 0;
  uint32_t nextAt_ = 0;
  Note     one_{0, 0, 0};
  uint8_t  pin_ = BUZZER_PIN;
  enum Mode : uint8_t { SEQ, CHORD };
  Mode     mode_  = SEQ;
  const uint16_t *chordFreqs_ = nullptr;
  uint8_t  chordN_ = 0;
  uint16_t chordIdx_ = 0, chordStepMs_ = 11, chordTotal_ = 0;
  uint32_t chordStart_ = 0;
  uint16_t curHz_  = 0;
  uint8_t  curLvl_ = 0;
  bool     inGap_ = false;
  bool     muted_ = false;
  uint8_t  volume_ = 7;
};

// ── Tuned to the part, and to a scale ──────────────────────────────────────
//
// This is the Axiometa Passive Buzzer AX22-0018: a piezo resonator specified at
// 2.7 kHz / 80 dB, whose output falls away steeply either side of resonance.
// Two constraints therefore fight each other, and both have been got wrong here
// already:
//
//   * the first melodies were written on a musical scale from 440 Hz to
//     1.76 kHz -- an octave or two below the only band this part is loud in,
//     so an alarm was inaudible across a room;
//   * the replacements sat in the right band but on arbitrary round numbers
//     (2100, 2450, 2750, 3100 Hz), which are not intervals. Loud, and harsh.
//
// Octave 7 resolves both: real equal-tempered pitches that happen to straddle
// 2.7 kHz. C7-E7-G7 is a C major triad AND sits where the buzzer is loudest.
static const uint16_t N_C7 = 2093, N_D7 = 2349, N_E7 = 2637;
static const uint16_t N_F7 = 2794, N_G7 = 3136, N_A7 = 3520;
// F# major, for the startup chime.
static const uint16_t N_CS7 = 2217, N_FS7 = 2960, N_AS7 = 3729;

// Short notes, real intervals, and a fade instead of a cut. A piezo held on one
// pitch for half a second drones; the same pitch struck and released rings.
//

// The startup inverted: the same triad descending, slowing and fading out.
const Note Buzzer::MEL_SLEEP[5] = {
  {N_G7,90,7},{N_E7,100,5},{N_C7,130,3},{0,40,0},{N_C7,170,1},
};

// A falling fifth, repeated. An interval rather than two unrelated tones, so it
// reads as a signal instead of a malfunction -- and still unmistakably urgent.
const Note Buzzer::MEL_WARN[7] = {
  {N_G7,100,10},{N_C7,100,10},{0,45,0},
  {N_G7,100,10},{N_C7,100,10},{0,45,0},{N_G7,130,10},
};

// A rising major triad at half level. It fires on every recovery, so it has to
// be small: a fanfare would become an irritation the first time a network
// flapped.
const Note Buzzer::MEL_HEALTHY[3] = {{N_C7,70,5},{N_E7,70,5},{N_G7,110,5}};

// Two clipped blips and a fall -- something failing to catch. Deliberately not
// the WARN interval: losing the broker is not an environmental alarm, and
// confusing the two sends someone to the wrong room.
const Note Buzzer::MEL_NOCONN[5] = {
  {N_E7,70,6},{0,55,0},{N_E7,70,6},{0,55,0},{N_C7,150,4},
};

const Note Buzzer::MEL_RH[4]    = {{N_G7,90,10},{0,55,0},{N_G7,90,10},{0,240,0}};
const Note Buzzer::MEL_TEMP[3]  = {{N_C7,80,9},{N_E7,80,9},{N_G7,130,9}};
const Note Buzzer::MEL_DEAD[3]  = {{N_G7,130,8},{N_E7,130,6},{N_C7,280,4}};
const Note Buzzer::MEL_HOUR[3]  = {{N_C7,70,7},{N_E7,70,7},{N_G7,130,7}};
const Note Buzzer::MEL_SAVED[2] = {{N_E7,70,6},{N_G7,110,6}};
const Note Buzzer::MEL_CLICK[1] = {{N_E7,25,5}};
const Note Buzzer::MEL_WELCOME[4] = {{N_C7,90,7},{N_E7,90,7},{N_G7,90,7},{N_C7,160,5}};
