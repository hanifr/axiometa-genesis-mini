#pragma once
// annunciator.h — the alert surface: 5x5 addressable matrix plus buzzer.
//
// They live together because an alarm is ONE event with two expressions. Split
// across the loop they drifted: the buzzer sounded for conditions the LED did
// not show, and the hourly mark beeped three times while the matrix did
// nothing.
//
// Everything here is NON-BLOCKING, and that is the point. The previous hourly
// signal was `for (i<3) { pixel(); beep(2000,70); delay(120); }` -- 570 ms in
// which the USER button was not polled, the portal was not served and the
// watchdog was not fed. Long alarm patterns would have made that far worse.
// service() is called every loop and returns immediately.
//
// ⚠ POWER, and this one browns the board out rather than looking wrong.
// A WS2812 draws ~60 mA at full white. Twenty-five of them is ~1.5 A, several
// times what this board's USB or battery path can supply. The supply sags, the
// ESP32 resets, and the symptom is a random reboot -- nothing points at the
// LEDs. MAX_BRIGHT caps every channel: at 40/255 the worst case is
// 25 * 60 mA * 40/255 = 235 mA, which the board holds. Measure before raising.

#include <Adafruit_NeoPixel.h>
#include <Preferences.h>
#include <esp_system.h>
#include <math.h>
#include "pins.h"
#include "matrix_font.h"
#include "buzzer.h"

// 5x5 glyphs, written as five rows of five so they can be read as pictures.
// At file scope, not in the class: a constexpr member function cannot be called
// in a constant expression inside its own class body.
static constexpr uint32_t glyphRows(uint8_t r0, uint8_t r1, uint8_t r2,
                                    uint8_t r3, uint8_t r4) {
  return (uint32_t)r0 | ((uint32_t)r1 << 5) | ((uint32_t)r2 << 10) |
         ((uint32_t)r3 << 15) | ((uint32_t)r4 << 20);
}
//                            row bits are b4..b0 = left..right
static constexpr uint32_t GL_BANG = glyphRows(0b00100, 0b00100, 0b00100, 0b00000, 0b00100);
static constexpr uint32_t GL_X    = glyphRows(0b10001, 0b01010, 0b00100, 0b01010, 0b10001);
static constexpr uint32_t GL_UP   = glyphRows(0b00100, 0b01110, 0b11111, 0b00100, 0b00100);
static constexpr uint32_t GL_RING = glyphRows(0b01110, 0b10001, 0b10001, 0b10001, 0b01110);
static constexpr uint32_t GL_DOT  = glyphRows(0b00000, 0b00000, 0b00100, 0b00000, 0b00000);
static constexpr uint32_t GL_FULL = glyphRows(0b11111, 0b11111, 0b11111, 0b11111, 0b11111);
// Asymmetric on BOTH axes and different on every row -- the only shape that can
// tell a mirrored panel from a serpentine one. The cyan ring used earlier is
// symmetric horizontally AND unchanged by reversing any single row, so it
// confirmed nothing about either.
static constexpr uint32_t GL_TESTF = glyphRows(0b11110, 0b10000, 0b11100, 0b10000, 0b10000);

class Annunciator {
 public:
  enum Signal : uint8_t {
    SIG_IDLE,        // dim green breath -- proof of life, near-zero current
    SIG_RH_HIGH,     // "!"  amber, urgent double chirp
    SIG_TEMP_RISE,   // up arrow, red -- cooling has stopped
    SIG_SENSOR_DEAD, // "X"  red, slow toll: a fault, not a reading
    SIG_HOUR,        // blue sweep + three chirps, the hourly mark
    SIG_PORTAL,      // cyan ring, the setup AP is open
    SIG_BOOT,        // white sweep, once
    SIG_TEXT         // scrolling marquee; see scrollText()
  };

  void attach(Buzzer *b) { buz_ = b; }

  // The on-board RGB is driven through Adafruit_NeoPixel here rather than the
  // core's neopixelWrite(), because the two allocate RMT channels by different
  // routes and fight. The symptom was a log line every 2.5 s --
  //   E rmt: rmt_write_items(1119): RMT DRIVER ERR
  // -- once per sensor cycle, which is where the old pixel() call sat, with
  // the matrix silently failing to update. One library, two strips, no
  // contention.
  // The status colour is what the LED shows when nothing is sounding: green
  // for healthy, red for an alarm. Stored rather than written immediately, so
  // there is exactly ONE writer to this LED -- two writers on an addressable
  // LED means whichever ran last wins, which looks like a flicker nobody can
  // account for.
  void onboard(uint8_t r, uint8_t g, uint8_t b) {
    statusR_ = r; statusG_ = g; statusB_ = b;
  }

  // Follow the buzzer. Brightness is the note's level, so it breathes with the
  // envelope rather than blinking on and off, and hue comes from pitch: warm
  // low, green through the middle, cool high. A rising melody visibly warms
  // upward and the descending sleep chord cools -- the light carries the same
  // shape as the sound instead of merely flashing with it.
  //
  // Writes only when the value actually changes. show() on an addressable LED
  // is an RMT transaction; doing it every loop pass would be thousands a
  // second for no visible difference.
  // A slow breathe, in the manner of a sleeping Macintosh.
  //
  // The first version here was a cardiac lub-dub: two sharp beats and a pause
  // over two seconds. It read as urgent -- correct for a pulse, wrong for a
  // device sitting quietly in a window, where it looked like something
  // flashing for attention.
  //
  // This is the other idiom: one smooth rise and fall over five seconds that
  // lingers at the dim end and never reaches black. The squaring is what
  // produces the linger -- a plain cosine spends equal time everywhere and
  // reads as mechanical, while the squared curve hurries through the bright
  // part and dwells low, which is what makes it look like breathing rather
  // than a fader being moved.
  // Returns 0..255 as a pure fraction of the cycle. The floor is applied in
  // LED units by the caller, NOT here: folding it in at this scale and then
  // multiplying by a dim peak rounded it away entirely -- at peak 26 the
  // nominal floor of 5 became 5/255 x 26 = 0, so the LED really did go black
  // at the bottom of every breath while the constant claimed otherwise.
  static uint8_t breathe(uint32_t phase) {
    float s = 0.5f - 0.5f * cosf(2.0f * 3.14159265f * (float)phase / (float)BREATH_MS);
    return (uint8_t)(255.0f * s * s);        // squared: linger low, peak briefly
  }

  // Full-saturation hue to RGB. Picking three random bytes instead gives muddy
  // browns and near-blacks most of the time, because most of the RGB cube is
  // desaturated; walking the hue circle keeps every colour vivid.
  static void hueToRGB(uint16_t h, uint8_t peak, uint8_t &r, uint8_t &g, uint8_t &b) {
    h %= 360;
    uint8_t x = (uint8_t)((uint32_t)peak * (60 - (h % 60)) / 60);
    uint8_t y = (uint8_t)(peak - x);
    switch (h / 60) {
      case 0: r = peak; g = y;    b = 0;    break;
      case 1: r = x;    g = peak; b = 0;    break;
      case 2: r = 0;    g = peak; b = y;    break;
      case 3: r = 0;    g = x;    b = peak; break;
      case 4: r = y;    g = 0;    b = peak; break;
      default:r = peak; g = 0;    b = x;    break;
    }
  }

  // The idle look, in ONE place: the breath phase, the hue, and when the hue
  // changes. The centre LED and the matrix dot both ask for it, so they cannot
  // drift out of step -- two copies of this would be two rhythms that agree
  // only until somebody tunes one of them.
  //
  // `peak` differs between the two because the matrix is drawn through
  // setBrightness(MAX_BRIGHT), which scales everything by 40/255; the same
  // number would leave the dot four times dimmer than the LED beside it.
  void idleColour(uint32_t now, uint8_t peak, uint8_t &r, uint8_t &g, uint8_t &b) {
    uint32_t cycle = now / BREATH_MS;
    if (cycle != lastCycle_) {
      lastCycle_ = cycle;
      // At least 60 degrees from the last, or two consecutive draws land close
      // enough to look like nothing changed.
      hue_ = (uint16_t)((hue_ + 60 + (esp_random() % 240)) % 360);
    }
    uint8_t env   = breathe(now % BREATH_MS);
    uint8_t level = (uint8_t)(LED_FLOOR + ((uint32_t)(peak - LED_FLOOR) * env) / 255);
    hueToRGB(hue_, level, r, g, b);
  }

  void mirrorBuzzer(const Buzzer &bz) {
    // 40 Hz is far smoother than the eye needs and keeps show() -- an RMT
    // transaction -- off the hot path of every loop pass.
    uint32_t now = millis();
    if (now - lastMirror_ < 25) return;
    lastMirror_ = now;

    uint8_t r, g, b;
    if (bz.sounding()) {
      uint16_t hz = bz.hz();
      int32_t t = ((int32_t)hz - 2000) * 255 / 1700;   // 0 at 2 kHz, 255 at 3.7
      if (t < 0) t = 0; if (t > 255) t = 255;
      if (t < 128) {                       // amber -> green
        uint8_t k = (uint8_t)(t * 2);
        r = 255 - k; g = 60 + (uint8_t)((195 * k) / 255); b = 0;
      } else {                             // green -> blue
        uint8_t k = (uint8_t)((t - 128) * 2);
        r = 0; g = 255 - k; b = (uint8_t)((255 * k) / 255);
      }
      uint8_t lvl = bz.level() > 10 ? 10 : bz.level();
      r = (uint8_t)((uint16_t)r * lvl / 10);
      g = (uint8_t)((uint16_t)g * lvl / 10);
      b = (uint8_t)((uint16_t)b * lvl / 10);
      // The ceiling keeps a single LED at arm's length from being painful in a
      // dark corridor; it is a sign, not a torch.
      const uint8_t CAP = 90;
      if (r > CAP) r = CAP; if (g > CAP) g = CAP; if (b > CAP) b = CAP;
    } else if (sig_ != SIG_IDLE && sig_ != SIG_TEXT) {
      // Between the notes of a pattern the LED rests in THAT pattern's colour,
      // not the idle one. So the hourly mark leaves it blue for the length of
      // the mark and an alarm leaves it red, rather than snapping back to
      // green in every gap -- which read as a fault rather than a signal.
      const Pattern &p = PATTERNS[sig_];
      r = (uint8_t)(p.r / 3); g = (uint8_t)(p.g / 3); b = (uint8_t)(p.b / 3);
    } else {
      // Idle: a new hue every five seconds, beating.
      //
      // ⚠ This costs the LED its meaning at rest. Green-when-healthy let a
      // glance say "fine"; a random colour says only "running". That is an
      // acceptable trade here because the rhythm still carries liveness and
      // every condition that matters -- alarm, hour, portal -- overrides the
      // colour from the branch above. Do not also randomise those.
      // The colour changes at the TROUGH, where the LED is dimmest, so the
      // change is invisible and a colour lasts exactly one breath. A separate
      // timer changed it mid-breath at half brightness, which read as a glitch.
      idleColour(now, IDLE_PEAK, r, g, b);
    }
    if (r == lastR_ && g == lastG_ && b == lastB_) return;
    lastR_ = r; lastG_ = g; lastB_ = b;
    led_.setPixelColor(0, led_.Color(r, g, b));
    led_.show();
  }

  void loadGeometry() {
    Preferences p; p.begin("mxcfg", true);
    // Defaults are the VENDOR layout, decoded from Axiometa's own 5x5 example:
    // its heart array {1,3, 5..9, 10..14, 16..18, 22} renders an upright heart
    // under plain row*5+col, so the panel is row-major from the top-left and
    // not serpentine. Note what that array cannot prove: every one of its rows
    // is a palindrome, so it fixes row-major and the top origin but says
    // nothing about snaking or a horizontal mirror -- #MXWIRE settles those.
    // Defaults are what was MEASURED on this unit with #MXWIRE, not the
    // vendor's reference orientation: the module is mounted a half turn round,
    // so origin is bottom-right. The vendor's own 5x5 example decodes to
    // row-major from the top-left and not serpentine, which fixed the wiring
    // order; the half turn is how this one sits in its port.
    //
    // These are only the defaults for a device with empty NVS -- which is
    // exactly the state after an NVS erase, and getting them wrong there means
    // someone re-measures a mapping that was already known.
    serp_         = p.getBool("serp",   false);
    originRight_  = p.getBool("oright", true);
    originBottom_ = p.getBool("obot",   true);
    colMajor_     = p.getBool("colmaj", false);
    rtl_       = p.getBool("rtl", true);
    defStepMs_ = p.getUShort("step", 140);
    gapCols_   = p.getUChar("gap", 2);
    if (defStepMs_ < 30 || defStepMs_ > 1000) defStepMs_ = 140;
    if (gapCols_ > 5) gapCols_ = 5;
    p.end();
  }
  void setMapping(bool oRight, bool oBottom, bool colMajor, bool serp) {
    originRight_ = oRight; originBottom_ = oBottom;
    colMajor_ = colMajor;  serp_ = serp;
    Preferences p; p.begin("mxcfg", false);
    p.putBool("oright", oRight); p.putBool("obot", oBottom);
    p.putBool("colmaj", colMajor); p.putBool("serp", serp);
    p.end();
  }

  // Light LEDs by their position IN THE STRIP, bypassing index() entirely --
  // the point is to discover the mapping, so nothing may be applied on the
  // way out or the test measures its own assumption.
  void probeRaw(uint16_t from, uint16_t to, uint8_t r, uint8_t g, uint8_t b) {
    apply(SIG_IDLE);
    nextAt_ = millis() + 60000;
    px_.clear();
    for (uint16_t i = from; i <= to && i < MATRIX_LEDS; i++)
      px_.setPixelColor(i, px_.Color(r, g, b));
    px_.show();
  }
  void setMarquee(uint16_t stepMs, uint8_t gap) {
    if (stepMs >= 30 && stepMs <= 1000) defStepMs_ = stepMs;
    if (gap <= 5) gapCols_ = gap;
    Preferences p; p.begin("mxcfg", false);
    p.putUShort("step", defStepMs_); p.putUChar("gap", gapCols_); p.end();
  }
  void setDirection(bool rtl) {
    rtl_ = rtl;
    Preferences p; p.begin("mxcfg", false); p.putBool("rtl", rtl); p.end();
  }
  bool rightToLeft() const { return rtl_; }

  uint16_t marqueeStep() const { return defStepMs_; }
  uint8_t  marqueeGap()  const { return gapCols_; }

  bool serpentine()   const { return serp_; }
  bool originRight()  const { return originRight_; }
  bool originBottom() const { return originBottom_; }
  bool colMajor()     const { return colMajor_; }

  // Hold a still shape while someone looks at the panel, without the pattern
  // engine repainting over it.
  void showStill(uint32_t glyph, uint8_t r, uint8_t g, uint8_t b) {
    apply(SIG_IDLE);
    nextAt_ = millis() + 60000;
    draw(glyph, r, g, b, 0);
  }
  static uint32_t testGlyph() { return GL_TESTF; }

  void begin() {
    led_.begin();
    led_.setBrightness(255);   // its own values are already small
    led_.clear();
    led_.show();
    px_.begin();
    px_.setBrightness(MAX_BRIGHT);
    px_.clear();
    px_.show();
    set(SIG_BOOT);
  }

  // Idempotent: re-asserting the signal already showing does not restart it,
  // so an alarm that stays true does not stutter every time it is re-detected.
  //
  // Automatic callers are ignored while a bench signal is held. Without that,
  // #SIG was unusable: the sensor loop stands alarm patterns down within 2.5 s
  // whenever no alarm is genuinely true, so a pattern set by hand vanished
  // before it could be looked at.
  void set(Signal s) {
    if (millis() < manualUntil_) return;
    apply(s);
  }

  // Bench override. Holds for long enough to watch a full pattern, then
  // automatic control resumes on its own -- a diagnostic that latches forever
  // is a diagnostic someone forgets to turn off.
  void setManual(Signal s, uint32_t holdMs = 30000) {
    manualUntil_ = millis() + holdMs;
    apply(s);
  }

  uint32_t manualHoldRemaining() const {
    uint32_t now = millis();
    return now < manualUntil_ ? manualUntil_ - now : 0;
  }

  Signal current() const { return sig_; }

  void service() {
    uint32_t now = millis();
    if ((int32_t)(now - nextAt_) < 0) return;

    if (sig_ == SIG_TEXT) { serviceText(now); return; }

    // Idle is drawn continuously, not blinked from the pattern table. It used
    // to be a fixed dim green dot on 1.2 s and off 2.4 s, which is a different
    // rhythm from the LED beside it -- two things saying "alive" out of time
    // with each other read as two unrelated signals.
    if (sig_ == SIG_IDLE) { serviceIdle(now); return; }

    const Pattern &p = PATTERNS[sig_];

    // Odd steps are the gap; even steps show the glyph. Encoding it this way
    // means one table drives blink rate, tone and colour together and they
    // cannot fall out of step with each other.
    bool on = (step_ % 2) == 0;
    draw(on ? p.glyph : 0, p.r, p.g, p.b, on ? frameShift(p) : 0);

    // Audio is a melody on its own clock, not one beep per blink. Tying a tone
    // to the blink rate made every alarm sound the same -- a fast beep or a
    // slow one -- and the blink rate is chosen for how it LOOKS.
    if (p.mel && p.melEveryMs && (int32_t)(now - melNextAt_) >= 0) {
      if (buz_) buz_->play(p.mel, p.melLen);
      melNextAt_ = now + p.melEveryMs;
    }

    nextAt_ = now + (on ? p.onMs : p.offMs);
    step_++;

    // A one-shot pattern falls back to idle instead of latching. An alarm
    // pattern repeats forever; it is cleared by the caller when the condition
    // clears, never by a timer, or the board would go quiet while still hot.
    if (p.cycles && step_ >= p.cycles * 2) apply(SIG_IDLE);
  }

  // Scroll a message across the panel, one column per step.
  //
  // Deliberately NOT held as a manual signal: a real alarm must be able to cut
  // a greeting short. Five columns of lead-in and lead-out mean the text
  // enters and leaves cleanly instead of appearing mid-letter.
  // stepMs 0 means "use the stored default", so call sites do not each carry
  // their own copy of the speed.
  void scrollText(const char *msg, uint8_t r, uint8_t g, uint8_t b,
                  uint16_t stepMs = 0, uint8_t loops = 1) {
    txtLen_ = 0;
    for (const char *c = msg; *c && txtLen_ < MAX_COLS - 6; c++) {
      const Glyph *gl = find(*c);
      if (!gl) continue;
      for (uint8_t i = 0; i < gl->width && txtLen_ < MAX_COLS - 1; i++)
        txtCols_[txtLen_++] = gl->col[i];
      // Inter-character gap. One blank column ran the letters together on a
      // panel only five wide -- at any moment you see parts of two characters,
      // so the gap is the only thing separating them. At the default step this
      // is the "delay between characters"; widen it rather than slowing the
      // whole message down, which makes a long greeting tedious.
      for (uint8_t gcol = 0; gcol < gapCols_ && txtLen_ < MAX_COLS - 1; gcol++)
        txtCols_[txtLen_++] = 0;
    }
    txtR_ = r; txtG_ = g; txtB_ = b;
    txtStepMs_ = stepMs ? stepMs : defStepMs_;
    txtLoops_ = loops ? loops : 1;
    txtPos_ = -5;                            // start off the right-hand edge
    manualUntil_ = 0;                        // an alarm may interrupt a greeting
    apply(SIG_TEXT);
  }

  // Light every LED on a nominated pin, to find which line a module's data
  // input actually sits on.
  //
  // A pull-up loading test cannot find a WS2812: its DIN is a high-impedance
  // CMOS gate, so it reads "open" whether the module is present or not. The
  // only way to identify the data line is to drive it and look.
  void probePin(uint8_t gpio, uint8_t r, uint8_t g, uint8_t b) {
    sig_ = SIG_IDLE;                  // stop the pattern fighting the probe
    nextAt_ = millis() + 4000;
    if (buz_) buz_->silence();
    px_.setPin(gpio);
    px_.clear();
    for (uint16_t i = 0; i < MATRIX_LEDS; i++) px_.setPixelColor(i, px_.Color(r, g, b));
    px_.show();
  }

  void probeEnd() {
    px_.setPin(MATRIX_PIN);
    px_.clear();
    px_.show();
    apply(SIG_BOOT);
  }

  // Bench control, so a pattern can be checked without waiting for weather.
  static const char *name(Signal s) {
    static const char *N[] = {"idle","rh-high","temp-rise","sensor-dead",
                              "hour","portal","boot","text"};
    return N[s];
  }

 private:
  static const uint16_t MAX_COLS = 192;      // ~38 characters at 5 columns

  static const Glyph *find(char c) {
    if (c >= 'a' && c <= 'z') c -= 32;       // the font is uppercase only
    for (uint8_t i = 0; i < FONT_COUNT; i++)
      if (FONT[i].ch == c) return &FONT[i];
    return nullptr;
  }

  // The centre dot, breathing in step with the LED and in the same colour.
  void serviceIdle(uint32_t now) {
    uint8_t r, g, b;
    idleColour(now, MATRIX_IDLE_PEAK, r, g, b);
    if (r == idleR_ && g == idleG_ && b == idleB_) { nextAt_ = now + 40; return; }
    idleR_ = r; idleG_ = g; idleB_ = b;
    px_.clear();
    px_.setPixelColor(index(2, 2), px_.Color(r, g, b));
    px_.show();
    nextAt_ = now + 40;               // 25 Hz: smooth, and 25 LEDs is a real write
  }

  void serviceText(uint32_t now) {
    px_.clear();
    for (uint8_t col = 0; col < 5; col++) {
      // Travel direction is SEPARATE from the panel mapping. While the wiring
      // was unknown the two looked like one transform -- mirroring the panel
      // reverses apparent travel -- so this started as a single flag. Once
      // #MXWIRE pinned the geometry that stopped being true: the mapping is
      // now a measured fact about the hardware and the direction is a
      // preference, and folding them together means fixing one breaks the
      // other. Two settings, because there are two things.
      int16_t src = txtPos_ + (rtl_ ? col : (4 - col));
      if (src < 0 || src >= (int16_t)txtLen_) continue;
      uint8_t bits = txtCols_[src];
      for (uint8_t row = 0; row < 5; row++)
        if (bits & (1 << row))
          px_.setPixelColor(index(row, col), px_.Color(txtR_, txtG_, txtB_));
    }
    px_.show();
    nextAt_ = now + txtStepMs_;
    if (++txtPos_ > (int16_t)txtLen_) {
      if (--txtLoops_) txtPos_ = -5;          // round again
      else             apply(SIG_IDLE);
    }
  }

  void apply(Signal s) {
    if (s == sig_) return;
    sig_ = s;
    step_ = 0;
    nextAt_ = millis();
    if (buz_) buz_->silence();
    // Announce on entry, so the first thing heard is the alarm's own contour
    // rather than up to melEveryMs of silence.
    const Pattern &p = PATTERNS[s];
    if (p.mel && buz_) buz_->play(p.mel, p.melLen);
    melNextAt_ = millis() + (p.melEveryMs ? p.melEveryMs : 0xFFFFFFF);
  }

  // At 40/255 the matrix is clearly readable across a room and safely within
  // the supply. See the power note at the top before changing this.
  static const uint8_t MAX_BRIGHT = 40;

  struct Pattern {
    uint32_t glyph;      // 25 bits, row 0 = top, bit 0 = top-left
    uint8_t  r, g, b;
    const Note *mel;     // nullptr = silent
    uint8_t  melLen;
    uint16_t melEveryMs; // 0 = play once on entry, never repeat
    uint16_t onMs, offMs;
    uint8_t  cycles;     // 0 = repeat until cleared
    bool     sweep;      // animate the glyph across columns instead of blinking
  };

  // Indexed by Signal, in enum order.
  static const Pattern PATTERNS[8];

  uint8_t frameShift(const Pattern &p) const {
    return p.sweep ? (step_ / 2) % 5 : 0;
  }

  // Panel wiring, IDENTIFIED rather than guessed, and persisted.
  //
  // The first model here had two degrees of freedom -- snaking and a
  // horizontal mirror -- and none of its four combinations drew a correct
  // glyph, because a panel may also be wired COLUMN-major (the strip runs down
  // columns, not across rows) and may start at any of the four corners. That
  // is sixteen orientations, too many to judge by eye one at a time, so
  // #MXWIRE identifies them from three observations instead:
  //
  //   originBottom_/originRight_  which corner holds LED 0
  //   colMajor_                   whether LEDs 0-4 form a column or a row
  //   serp_                       whether the second run starts far or near
  //
  // Three questions settle all sixteen.
  uint16_t index(uint8_t row, uint8_t col) const {
    uint8_t r = originBottom_ ? (uint8_t)(4 - row) : row;
    uint8_t c = originRight_  ? (uint8_t)(4 - col) : col;
    uint8_t run, pos;
    if (colMajor_) { run = c; pos = r; }   // the strip runs down columns
    else           { run = r; pos = c; }   // ...or across rows
    if (serp_ && (run & 1)) pos = 4 - pos; // every other run doubles back
    return run * 5 + pos;
  }

  void draw(uint32_t glyph, uint8_t r, uint8_t g, uint8_t b, uint8_t shift) {
    px_.clear();
    for (uint8_t row = 0; row < 5; row++) {
      for (uint8_t col = 0; col < 5; col++) {
        uint8_t src = shift ? (col + shift) % 5 : col;
        // bit 4 is the leftmost column, so column c is bit (4 - c)
        if (glyph & (1UL << (row * 5 + (4 - src))))
          px_.setPixelColor(index(row, col), px_.Color(r, g, b));
      }
    }
    px_.show();
  }


  Adafruit_NeoPixel px_{MATRIX_LEDS, MATRIX_PIN, NEO_GRB + NEO_KHZ800};
  Adafruit_NeoPixel led_{1, PIN_RGB, NEO_GRB + NEO_KHZ800};
  uint8_t statusR_ = 0, statusG_ = 12, statusB_ = 0;
  uint8_t lastR_ = 255, lastG_ = 255, lastB_ = 255;   // force a first write
  uint32_t lastMirror_ = 0;
  // A single LED at arm's length in a corridor: bright enough to notice, not
  // to stare into.
  static const uint8_t IDLE_PEAK = 26;
  // Nine seconds a cycle, and dim. A sign in a window is looked at, not
  // stared into: the resting LED should be noticeable in peripheral vision and
  // invisible as a light source. A floor rather than black, because an LED
  // that goes fully out reads as switched off rather than resting.
  static const uint32_t BREATH_MS = 6500;
  static const uint8_t  LED_FLOOR  = 2;    // in LED units, so it cannot round away
  // Higher than the LED's peak because setBrightness(MAX_BRIGHT) scales the
  // matrix by 40/255 on the way out.
  static const uint8_t MATRIX_IDLE_PEAK = 170;
  uint8_t  idleR_ = 255, idleG_ = 255, idleB_ = 255;   // force a first draw
  uint16_t hue_       = 0;
  uint32_t lastCycle_ = 0xFFFFFFFF;   // force a colour on the first breath
  Buzzer  *buz_        = nullptr;
  bool     serp_         = true;
  bool     originRight_  = false;
  bool     originBottom_ = false;
  bool     colMajor_     = false;
  bool     rtl_        = true;    // text travels right to left, as read
  uint16_t defStepMs_  = 140;
  uint8_t  gapCols_    = 2;
  uint32_t melNextAt_  = 0;
  uint8_t  txtCols_[MAX_COLS] = {0};
  uint16_t txtLen_     = 0;
  int16_t  txtPos_     = 0;
  uint8_t  txtR_ = 0, txtG_ = 0, txtB_ = 0;
  uint16_t txtStepMs_  = 110;
  uint8_t  txtLoops_   = 1;
  Signal   sig_        = SIG_IDLE;
  uint32_t manualUntil_ = 0;
  uint8_t  step_    = 0;
  uint32_t nextAt_  = 0;
};

// glyph        r    g    b   melody              len  every   on   off  cyc sweep
const Annunciator::Pattern Annunciator::PATTERNS[8] = {
  { GL_DOT,     0,  12,   0,  nullptr,             0,     0, 1200, 2400,  0, false },
  { GL_BANG,  255, 140,   0,  Buzzer::MEL_RH,      4,  3000,  180,  180,  0, false },
  { GL_UP,    255,  30,   0,  Buzzer::MEL_TEMP,    3,  4000,  260,  260,  0, false },
  { GL_X,     255,   0,   0,  Buzzer::MEL_DEAD,    3,  6000,  600, 1400,  0, false },
  { GL_FULL,    0,  60, 255,  Buzzer::MEL_HOUR,    3,     0,  120,  120,  3, true  },
  { GL_RING,    0, 180, 200,  nullptr,             0,     0,  500,  500,  0, false },
  { GL_FULL,  120, 120, 120,  nullptr,             0,     0,   90,   40,  5, true  },
  // SIG_TEXT never reads this row -- service() branches to serviceText()
  // before touching the table -- but the array is indexed by Signal and must
  // cover every enumerator, or adding one silently reads past the end.
  { 0,          0,   0,   0,  nullptr,             0,     0,  100,  100,  0, false },
};
