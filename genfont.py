# Generates matrix_font.h from ASCII art, then renders the target message back
# through the same packing so the glyphs are checked, not trusted.
ART = {
 'A': [".#.", "#.#", "###", "#.#", "#.#"],
 'B': ["##.", "#.#", "##.", "#.#", "##."],
 'C': [".##", "#..", "#..", "#..", ".##"],
 'D': ["##.", "#.#", "#.#", "#.#", "##."],
 'E': ["###", "#..", "##.", "#..", "###"],
 'F': ["###", "#..", "##.", "#..", "#.."],
 'G': [".##", "#..", "#.#", "#.#", ".##"],
 'H': ["#.#", "#.#", "###", "#.#", "#.#"],
 'I': ["###", ".#.", ".#.", ".#.", "###"],
 'J': ["..#", "..#", "..#", "#.#", ".#."],
 'K': ["#.#", "#.#", "##.", "#.#", "#.#"],
 'L': ["#..", "#..", "#..", "#..", "###"],
 'M': ["#...#", "##.##", "#.#.#", "#...#", "#...#"],
 'N': ["#..#", "##.#", "#.##", "#..#", "#..#"],
 'O': [".#.", "#.#", "#.#", "#.#", ".#."],
 'P': ["##.", "#.#", "##.", "#..", "#.."],
 'Q': [".#.", "#.#", "#.#", "##.", ".##"],
 'R': ["##.", "#.#", "##.", "#.#", "#.#"],
 'S': [".##", "#..", ".#.", "..#", "##."],
 'T': ["###", ".#.", ".#.", ".#.", ".#."],
 'U': ["#.#", "#.#", "#.#", "#.#", ".#."],
 'V': ["#.#", "#.#", "#.#", ".#.", ".#."],
 'W': ["#...#", "#...#", "#.#.#", "##.##", "#...#"],
 'X': ["#.#", "#.#", ".#.", "#.#", "#.#"],
 'Y': ["#.#", "#.#", ".#.", ".#.", ".#."],
 'Z': ["###", "..#", ".#.", "#..", "###"],
 '0': ["###", "#.#", "#.#", "#.#", "###"],
 '1': [".#.", "##.", ".#.", ".#.", "###"],
 '2': ["###", "..#", "###", "#..", "###"],
 '3': ["###", "..#", "###", "..#", "###"],
 '4': ["#.#", "#.#", "###", "..#", "..#"],
 '5': ["###", "#..", "###", "..#", "###"],
 '6': ["###", "#..", "###", "#.#", "###"],
 '7': ["###", "..#", "..#", "..#", "..#"],
 '8': ["###", "#.#", "###", "#.#", "###"],
 '9': ["###", "#.#", "###", "..#", "###"],
 '!': [".#.", ".#.", ".#.", "...", ".#."],
 '.': ["...", "...", "...", "...", ".#."],
 '-': ["...", "...", "###", "...", "..."],
 ' ': ["..", "..", "..", "..", ".."],
}

def columns(art):
    w = len(art[0])
    assert all(len(r) == w for r in art), "ragged glyph"
    out = []
    for c in range(w):
        bits = 0
        for r in range(5):
            if art[r][c] == '#':
                bits |= 1 << r          # bit 0 = top row
        out.append(bits)
    return out

order = sorted(ART)
lines = []
lines.append("#pragma once")
lines.append("// matrix_font.h - 5-row variable-width font for the 5x5 matrix marquee.")
lines.append("//")
lines.append("// GENERATED from ASCII art by tools/genfont.py. Each glyph is a run of")
lines.append("// columns, one byte per column, bit 0 = top row. Widths vary: M and W need")
lines.append("// five columns and look wrong squeezed into three, while I and L look wrong")
lines.append("// padded out to five. Do not hand-edit -- edit the art and regenerate.")
lines.append("")
lines.append("struct Glyph { char ch; uint8_t width; uint8_t col[5]; };")
lines.append("")
lines.append("static const Glyph FONT[] = {")
for ch in order:
    cols = columns(ART[ch])
    padded = cols + [0] * (5 - len(cols))
    esc = {"'": "\\'", "\\": "\\\\"}.get(ch, ch)
    lines.append("  { '%s', %d, {%s} }," % (esc, len(cols),
                 ", ".join("0x%02X" % c for c in padded)))
lines.append("};")
lines.append("static const uint8_t FONT_COUNT = sizeof(FONT) / sizeof(FONT[0]);")
lines.append("")
open("matrix_font.h", "w").write("\n".join(lines) + "\n")
print("matrix_font.h: %d glyphs" % len(order))

# ── Verify: rebuild the message column buffer and render it ─────────────────
MSG = "GENESIS MINI READY"
buf = []
for ch in MSG:
    g = ART.get(ch.upper())
    assert g, "no glyph for %r" % ch
    buf.extend(columns(g))
    buf.append(0)                        # one blank column between characters
print("\n  %r -> %d columns\n" % (MSG, len(buf)))
for r in range(5):
    print("  " + "".join("#" if (c >> r) & 1 else "." for c in buf))
