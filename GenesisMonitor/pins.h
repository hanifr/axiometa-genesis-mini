#pragma once
// pins.h — Axiometa Genesis Mini, as measured on this hardware.
//
// EVERY NUMBER HERE WAS PROBED, NOT READ OFF A DATASHEET, and the two differ.
//
// The AX22 connector labels each port's first GPIO "IO0/ADC", and the upstream
// variant header names them P<n>_IO0/1/2. Neither tells you which line a module
// actually drives. Measured on this unit:
//
//   DHT11   answers on the port's IO1, not IO0
//   slider  drives IO1 and IO2 (a dual-gang fader), and leaves IO0 flat at 0
//   display uses a DIFFERENT role mapping in port 1 than in port 2 --
//           CS=IO1,DC=IO0,RST=IO2 in port 1; CS=IO0,DC=IO2,RST=IO1 in port 2
//
// So module pin roles are neither "always IO0" nor portable between ports.
// Moving a module means re-probing. That is why the firmware keeps its wiring
// in NVS and offers #WIRE rather than computing pins from a port number.
//
// NEVER analogRead GPIO 19 or 20. soc/usb_pins.h: USBPHY_DM_NUM 19,
// USBPHY_DP_NUM 20 -- reading them reconfigures the native USB pins as ADC
// inputs and takes the board off the bus until it is power-cycled. It looks
// exactly like a failed cable.

// ── Board ───────────────────────────────────────────────────────────────────
#define PIN_MOSI      12
#define PIN_MISO      13
#define PIN_SCK       14
#define PIN_RGB       21      // onboard NeoPixel
#define PIN_USER      45      // onboard button. NOT an RTC pin: GPIO 0-21 only,
                              // so it cannot wake the board from deep sleep.
#define PIN_BAT_SENSE  8      // divider /2. The variant's getBatteryVoltage()
                              // macro is broken upstream -- it references an
                              // undefined VBAT_SENSE -- so read the pin.

// ── Port table ──────────────────────────────────────────────────────────────
// Matches espressif/arduino-esp32 variants/axiometa_genesis_mini/pins_arduino.h
// exactly, checked against it after probing this unit independently. Module
// pins below are written in terms of these, so moving a module is a one-line
// change instead of a hunt.
//
// ⚠ ADC2 pins cannot be analogRead while the radio is up -- they return junk,
// silently. On this board that is P3_IO1, P3_IO2, P4_IO1 and P4_IO2. An ANALOG
// module therefore belongs on a port's IO0, which is ADC1 on all four ports.
// Every in-firmware scan that "proved" the slider dead read an ADC2 pin with
// WiFi running; the readings were meaningless and cost most of a session.
#define P1_IO0         4      // ADC1  RTC  touch
#define P1_IO1         3      // ADC1  RTC  touch   (strapping: JTAG source)
#define P1_IO2         2      // ADC1  RTC  touch
#define P2_IO0         7      // ADC1  RTC  touch
#define P2_IO1         6      // ADC1  RTC  touch
#define P2_IO2         5      // ADC1  RTC  touch
#define P3_IO0         9      // ADC1  RTC  touch
#define P3_IO1        16      // ADC2  RTC         -- not analog-readable on WiFi
#define P3_IO2        15      // ADC2  RTC         -- not analog-readable on WiFi
#define P4_IO0         1      // ADC1  RTC  touch
#define P4_IO1        17      // ADC2  RTC         -- not analog-readable on WiFi
#define P4_IO2        18      // ADC2  RTC         -- not analog-readable on WiFi

// ── Modules, port by port ───────────────────────────────────────────────────
// Arrangement: 1 buzzer, 2 display, 3 EMPTY, 4 DHT11.
//
// Port 3 carries the 5x5 matrix, and it drives IO1. Established functionally
// with #MXSCAN (drive each line, see which lights), NOT by measurement:
//
//   IO0  gpio9   open                 -- unused by this module
//   IO1  gpio16  the WS2812 data line -- reads only as a "partial load"
//   IO2  gpio15  pulled low           -- unused by this module
//
// There is no fault in port 3. Two separate wrong conclusions were drawn here
// before the functional test settled it:
//
//   * with the slider plugged in, all three lines read 0 and the port looked
//     dead -- that module was tying them together;
//   * with the matrix plugged in, IO2 read like a dead short, because a WS2812
//     breakout puts a pull-down on DIN to keep it defined and that beats the
//     ESP32's ~45 kohm internal pull-up.
//
// A pull-up loading test cannot see a WS2812 at all (its DIN is a
// high-impedance CMOS gate) and misreads its pull-down as a short. Identify a
// digital module's pins by driving them, never by measuring them.
#define BUZZER_PIN     P1_IO1
// EVERY single-signal module on this board answers on IO1, not IO0: the DHT11,
// the 5x5 matrix and the passive buzzer, each confirmed by driving the line and
// observing, never by measurement. "IO0 is the analog/primary line" is what the
// AX22 labelling suggests and it was wrong three times out of three. The
// multi-pin display is the only module that uses IO0 at all.
//
// The buzzer's pin is ALSO stored in NVS (see buzzer.h) and the stored value
// wins; this define is only the factory default for a device with empty NVS.
#define LCD_CS         P2_IO0 // } measured; this module uses a DIFFERENT role
#define LCD_DC         P2_IO2 // } mapping in port 2 than it did in port 1
#define LCD_RST        P2_IO1 // }
#define DHT_PIN        P4_IO1 // IO1, not IO0 -- confirmed twice, in two ports
#define SLIDER_PIN     P3_IO0 // only read when USE_SLIDER is 1; see the .ino

// 5x5 addressable matrix, on port 3 IO1. Confirmed by #MXSCAN, which drives
// each of a port's lines in a different colour; the matrix answered on IO2.
// If the module is moved, re-run #MXSCAN rather than assuming -- of the three
// modules probed on this board, the DHT11 uses IO1, the buzzer IO0 and the
// matrix IO2, so there is no house rule to fall back on.
#define MATRIX_PIN     P3_IO1
#define MATRIX_LEDS    25
