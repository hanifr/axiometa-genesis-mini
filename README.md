# Genesis Mini firmware

Firmware for the [Axiometa Genesis Mini](https://www.axiometa.io/products/axiometa-genesis-mini)
(ESP32-S3, four AX22 ports), and — more usefully — **what the AX22 connector
actually does**, measured rather than assumed.

## What the AX22 pins really are

| AX22 line | GPIO | Notes |
|---|---|---|
| `P1_IO0` | 4 | ADC1  RTC  touch |
| `P1_IO1` | 3 | ADC1  RTC  touch   (strapping: JTAG source) |
| `P1_IO2` | 2 | ADC1  RTC  touch |
| `P2_IO0` | 7 | ADC1  RTC  touch |
| `P2_IO1` | 6 | ADC1  RTC  touch |
| `P2_IO2` | 5 | ADC1  RTC  touch |
| `P3_IO0` | 9 | ADC1  RTC  touch |
| `P3_IO1` | 16 | ADC2  RTC         -- not analog-readable on WiFi |
| `P3_IO2` | 15 | ADC2  RTC         -- not analog-readable on WiFi |
| `P4_IO0` | 1 | ADC1  RTC  touch |
| `P4_IO1` | 17 | ADC2  RTC         -- not analog-readable on WiFi |
| `P4_IO2` | 18 | ADC2  RTC         -- not analog-readable on WiFi |

| Module | Drives |
|---|---|
| BUZZER_PIN | `P1_IO1` |
| LCD_CS | `P2_IO0` |
| LCD_DC | `P2_IO2` |
| LCD_RST | `P2_IO1` |
| DHT_PIN | `P4_IO1` |
| SLIDER_PIN | `P3_IO0` |
| MATRIX_PIN | `P3_IO1` |

**Every single-signal module answers on IO1, not IO0.** The connector labels
IO0 "ADC" and the literature calls it the analog input, which invites the
opposite assumption. It was wrong for all three modules tested here: the
DHT11, the 5x5 matrix and the passive buzzer. Only the display, which drives
three lines, uses IO0 at all.

**ADC2 (GPIO 11–20) is unusable while WiFi is up**, and fails *silently* —
`analogRead` returns plausible small numbers. That covers `P3_IO1`, `P3_IO2`,
`P4_IO1` and `P4_IO2`. An analog module belongs on a port's IO0, which is ADC1
on all four ports.

## The schematics nobody links to

Axiometa publish a one-page KiCad schematic per module at:

    https://www.axiometa.io/cdn/shop/files/SCH_AX22-<code>.pdf

…where `<code>` is the product code from the shop page. They are not linked
from the specification tables and they answer in a minute what probing answers
badly over hours. Three worth knowing:

- **Sliding potentiometer (AX22-0020)** — 1 kΩ across Vcc/GND, wiper on **IO0**,
  and nothing else connected. A pin stuck at 0 that an internal pull-up cannot
  lift therefore has *no Vcc*.
- **Passive buzzer (AX22-0018)** — MLT-8530 driven by a MOSFET whose gate is
  **AC-coupled** (100 nF / 100 kΩ). A steady DC level can never make it sound,
  on any pin. Signal on **IO1**. Resonant at 2.7 kHz, so tones far from that
  are inaudible across a room.
- **LoRa (AX22-0057, LR1262)** — NSS←IO1, DIO1←IO0, BUSY←IO2, shared SPI, and
  **NRESET is not on the connector** (pull-up and a test pad only).

## Identify a module by driving it, not measuring it

A pull-up loading probe cannot find a digital module and will lie about what it
did find: an addressable LED's data input is high-impedance and reads "open",
its pull-down reads identically to a dead short, and a piezo is invisible. The
firmware carries diagnostics for this, usable on a bench with no instruments:

| Command | What it does |
|---|---|
| `#MXWIRE` | Lights raw strip positions to identify panel wiring |
| `#MXMAP,<r>,<b>,<c>,<s>` | Stores origin / major / serpentine mapping |
| `#BZID` | Drives each port pin with a distinct *number of beeps* |
| `#BZONE,<gpio>` | Drives one pin only — a single yes/no question |
| `#PORTZ,<1-4>` | Pull-up/pull-down loading, with its own caveats printed |

Answers are made self-identifying — a colour or a beep count, not "which step
looked right" — because the person watching the hardware is usually not the
person at the keyboard.

## Features

Environment monitoring (DHT11 → MQTT), an availability sign with a web page
served on the local network, a 5x5 matrix marquee and alert glyphs, composed
buzzer alerts tuned to the part's resonance, store-and-forward to flash across
broker outages, and a setup portal on its own access point.

Configuration lives in NVS and survives reflashing. Settings are versioned with
migrations guarded by `static_assert`.

## Building

Arduino ESP32 core **2.0.17**, board `esp32:esp32:esp32s3`, options
`CDCOnBoot=cdc,PSRAM=enabled,FlashSize=4M`.

Libraries: Adafruit GFX, Adafruit ST7735/ST7789, Adafruit NeoPixel, DHT sensor
library, PubSubClient.

> `Adafruit_NeoPixel` is LGPL-3.0; the rest are permissive. This project's own
> code is MIT (see `LICENSE`).

## Credit

Measured on one Axiometa Genesis Mini (ESP32-S3, four AX22 ports). Everything below was found by driving pins and watching modules, not by reading the labels. If your unit differs, #MXWIRE and #BZID in the firmware will tell you, and a correction is welcome.
