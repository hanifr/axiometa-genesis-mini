#pragma once
// store.h — readings kept on flash while the broker is unreachable.
//
// Before this, publish() opened with `if (!mqtt.connected()) return;` and every
// reading taken during an outage was discarded silently. A broker down for an
// afternoon left no trace anywhere: no gap marker, no counter, nothing.
//
// The 1.4 MB `spiffs` partition was sitting entirely unused. At one record per
// minute this holds roughly two months.
//
// ⚠ This is only correct because the reading carries its OWN timestamp. Buffer
// without that and a two-hour outage flushes as 120 readings all stamped within
// the same second -- a history that is confidently wrong, which is worse than
// the honest gap it replaced. See timeIsValid() in the sketch: nothing is
// queued while the clock is unset, for exactly that reason.

#include <LittleFS.h>

struct QRec {
  uint32_t ts;        // UTC epoch seconds
  float    t;
  float    rh;
  int16_t  rssi;
  uint16_t crc;       // over the 14 bytes before it
};
static_assert(sizeof(QRec) == 16, "QRec must stay 16 bytes: the file is an array of them");

class Store {
 public:
  bool begin() {
    // formatOnFail: an unformatted or corrupt partition is not a reason to
    // lose the whole feature, and there is nothing else on it to protect.
    ok_ = LittleFS.begin(true, "/littlefs", 10, "spiffs");
    if (!ok_) { Serial.println("#STORE,mount failed -- buffering disabled"); return false; }
    pending_ = fileRecords();
    Serial.printf("#STORE,mounted, %u record(s) pending, %u bytes free\n",
                  (unsigned)pending_, (unsigned)(LittleFS.totalBytes() - LittleFS.usedBytes()));
    return true;
  }

  bool ready() const { return ok_; }
  uint32_t pending() const { return pending_; }
  uint32_t dropped() const { return dropped_; }

  // Append one reading. Refuses rather than wrapping: dropping the OLDEST
  // needs a full rewrite on every append once full, and dropping the NEWEST
  // throws away the reading most likely to matter. Refusing and counting is
  // the only option that is both cheap and honest -- and at two months of
  // headroom, reaching it means something else has been wrong for weeks.
  bool push(const QRec &rec) {
    if (!ok_) return false;
    if (pending_ >= MAX_RECORDS) { dropped_++; return false; }
    File f = LittleFS.open(PATH, FILE_APPEND);
    if (!f) { dropped_++; return false; }
    QRec r = rec;
    r.crc = crc16((const uint8_t *)&r, sizeof(QRec) - 2);
    bool wrote = f.write((const uint8_t *)&r, sizeof(r)) == sizeof(r);
    f.close();
    if (wrote) pending_++; else dropped_++;
    return wrote;
  }

  // Hand up to `limit` records to `sink`, oldest first. Whatever the sink
  // accepts is consumed; the rest stays. The file is rewritten only when
  // something was actually sent, so a failing sink costs no flash writes.
  //
  // Records failing CRC are consumed and counted, not retried for ever: a torn
  // write from a power cut at the wrong moment would otherwise block the queue
  // permanently.
  uint32_t flush(bool (*sink)(const QRec &), uint32_t limit) {
    if (!ok_ || pending_ == 0) return 0;
    File f = LittleFS.open(PATH, FILE_READ);
    if (!f) return 0;

    uint32_t sent = 0, bad = 0;
    QRec r;
    while (sent < limit && f.read((uint8_t *)&r, sizeof(r)) == sizeof(r)) {
      if (crc16((const uint8_t *)&r, sizeof(QRec) - 2) != r.crc) { bad++; sent++; continue; }
      if (!sink(r)) break;
      sent++;
    }
    if (sent == 0) { f.close(); return 0; }

    // Copy the remainder forward, then swap. A rewrite rather than an in-place
    // shift because LittleFS has no truncate-from-the-front.
    size_t keepFrom = (size_t)sent * sizeof(QRec);
    f.seek(keepFrom);
    File out = LittleFS.open(TMP, FILE_WRITE);
    if (out) {
      uint8_t buf[256];
      int n;
      while ((n = f.read(buf, sizeof(buf))) > 0) out.write(buf, n);
      out.close();
    }
    f.close();
    LittleFS.remove(PATH);
    LittleFS.rename(TMP, PATH);
    pending_ = fileRecords();
    badRecords_ += bad;
    return sent;
  }

  uint32_t badRecords() const { return badRecords_; }

  void clear() {
    if (!ok_) return;
    LittleFS.remove(PATH);
    pending_ = 0;
  }

 private:
  static constexpr const char *PATH = "/q.bin";
  static constexpr const char *TMP  = "/q.tmp";
  // ~87 000 records fit in 1.4 MB; cap well below that so a rewrite stays
  // quick and the partition keeps room for the temporary copy.
  static const uint32_t MAX_RECORDS = 40000;

  uint32_t fileRecords() {
    File f = LittleFS.open(PATH, FILE_READ);
    if (!f) return 0;
    uint32_t n = f.size() / sizeof(QRec);
    f.close();
    return n;
  }

  // CRC16/CCITT-FALSE. Present to catch a torn write, not tampering.
  static uint16_t crc16(const uint8_t *d, size_t n) {
    uint16_t c = 0xFFFF;
    for (size_t i = 0; i < n; i++) {
      c ^= (uint16_t)d[i] << 8;
      for (int b = 0; b < 8; b++) c = (c & 0x8000) ? (uint16_t)((c << 1) ^ 0x1021) : (uint16_t)(c << 1);
    }
    return c;
  }

  bool     ok_ = false;
  uint32_t pending_ = 0, dropped_ = 0, badRecords_ = 0;
};
