#!/usr/bin/env python3
"""Writes the built-in phrases for StageRig's Phrase effect: short MIDI files,
one bar or less, in three folders. Licks and Comping are written from C4, so
"begins on the key pressed" moves them to any key; Drums use General MIDI
drum notes on channel 10 and are played with the pitch fixed. Run it from
this folder to regenerate them: python3 make-phrases.py"""
import os, struct

TPQ = 480

def vlq(n):
    out = [n & 0x7f]; n >>= 7
    while n: out.append((n & 0x7f) | 0x80); n >>= 7
    return bytes(reversed(out))

def write(path, notes, channel=1):
    """notes: (beat, length in beats, note, velocity)."""
    events = []
    for beat, length, note, vel in notes:
        events.append((round(beat * TPQ), 1, 0x90 | (channel - 1), note, vel))
        events.append((round((beat + length) * TPQ), 0, 0x80 | (channel - 1), note, 0))
    events.sort(key=lambda e: (e[0], e[1]))      # offs before ons at the same tick
    track = b"\x00\xff\x51\x03" + (500000).to_bytes(3, "big")           # 120 bpm
    at = 0
    for tick, _, status, note, vel in events:
        track += vlq(tick - at) + bytes([status, note, vel]); at = tick
    track += b"\x00\xff\x2f\x00"
    data = b"MThd" + struct.pack(">IHHH", 6, 0, 1, TPQ) + b"MTrk" + struct.pack(">I", len(track)) + track
    os.makedirs(os.path.dirname(path), exist_ok=True)
    open(path, "wb").write(data)

C = 60
def seq(notes, length, start=0.0, step=None, vel=100):
    """Notes one after another, `length` beats each (or `step` apart)."""
    step = length if step is None else step
    return [(start + i * step, length, n, vel) for i, n in enumerate(notes)]
def chord(beat, length, notes, vel=96):
    return [(beat, length, n, vel) for n in notes]

licks = {
    "Trill":              seq([C, C + 2] * 4, 0.125),
    "Trill (minor 2nd)":  seq([C, C + 1] * 4, 0.125),
    "Mordent":            [(0, 0.125, C, 100), (0.125, 0.125, C + 2, 90), (0.25, 0.75, C, 100)],
    "Turn":               [(0, 0.125, C + 2, 90), (0.125, 0.125, C, 100), (0.25, 0.125, C - 1, 90), (0.375, 0.625, C, 100)],
    "Gliss up an octave": seq(list(range(C, C + 13)), 0.08, step=1.0 / 12.0),
    "Gliss down an octave": seq(list(range(C, C - 13, -1)), 0.08, step=1.0 / 12.0),
    "Pentatonic run up":  seq([C, C + 2, C + 4, C + 7, C + 9, C + 12, C + 14, C + 16, C + 19, C + 21, C + 24], 0.2, step=0.2),
    "Blues lick":         [(0, 0.5, C, 100), (0.5, 0.25, C + 3, 100), (0.75, 0.25, C + 5, 100), (1.0, 0.25, C + 6, 110), (1.25, 0.25, C + 7, 100), (1.5, 0.5, C + 10, 100), (2.0, 2.0, C + 12, 110)],
    "Blues turnaround":   [(0, 0.5, C + 12, 100), (0.5, 0.5, C + 11, 100), (1.0, 0.5, C + 10, 100), (1.5, 0.5, C + 9, 100), (2.0, 1.0, C + 7, 100), (3.0, 1.0, C + 7 + 12, 110)],
    "Horn stab":          chord(0, 0.3, [C, C + 4, C + 7, C + 12], 112),
    "Horn fall":          seq([C + 12, C + 11, C + 10, C + 9, C + 8, C + 7, C + 5], 0.12, step=0.125, vel=110),
    "Octave bounce":      seq([C, C + 12] * 4, 0.4, step=0.5),
    "Bass walk (major)":  seq([C - 12, C - 8, C - 5, C - 3], 0.9, step=1.0, vel=104),
    "Bass walk (minor)":  seq([C - 12, C - 9, C - 5, C - 2], 0.9, step=1.0, vel=104),
    "Rock 8ths":          seq([C - 12] * 8, 0.4, step=0.5, vel=104),
    "Arpeggio up and down": seq([C, C + 4, C + 7, C + 12, C + 7, C + 4], 0.3, step=1.0 / 3.0) + [(2.0, 2.0, C, 100)],
}
comping = {
    "Major stab":         chord(0, 0.25, [C, C + 4, C + 7, C + 12], 104),
    "Minor 7 stab":       chord(0, 0.25, [C, C + 3, C + 7, C + 10], 104),
    "Dominant 7 stab":    chord(0, 0.25, [C, C + 4, C + 7, C + 10], 104),
    "Shuffle comp (dom 7)": sum([chord(b, 0.3, [C, C + 4, C + 7, C + 10], 96 if i % 2 == 0 else 84) for i, b in enumerate([0, 2/3, 1, 5/3, 2, 8/3, 3, 11/3])], []),
    "Reggae skank (minor)": sum([chord(b, 0.2, [C, C + 3, C + 7], 100) for b in (0.5, 1.5, 2.5, 3.5)], []),
    "Funk 16ths (dom 9)": sum([chord(i * 0.25, 0.1, [C + 4, C + 7, C + 10, C + 14], 100 if i % 4 == 0 else 80) for i in range(16) if i not in (2, 6, 10, 14)], []),
    "Ballad 8ths (major)": sum([chord(i * 0.5, 0.45, [C, C + 4, C + 7], 80 if i % 2 else 96) for i in range(8)], []),
    "Bossa comp (maj 7)": sum([chord(b, 0.22, [C + 4, C + 7, C + 11, C + 14], 92) for b in (0, 0.75, 1.5, 2.5, 3.0, 3.75)], []),
    "Pad swell (major)":  chord(0, 4.0, [C, C + 4, C + 7, C + 12], 90),
}
K, S, H, OH, R, CR, RIM, LT, HT, PH = 36, 38, 42, 46, 51, 49, 37, 45, 48, 44
def drums(hits):
    """hits: (beat, note, velocity); every hit a tenth of a beat long."""
    return [(b, 0.1, n, v) for b, n, v in hits]
hats8 = [(i * 0.5, H, 90 if i % 2 == 0 else 70) for i in range(8)]
hats16 = [(i * 0.25, H, 90 if i % 4 == 0 else 66 if i % 2 else 78) for i in range(16)]
drum_patterns = {
    "Rock 8ths":          drums(hats8 + [(0, K, 110), (2, K, 110), (1, S, 110), (3, S, 110)]),
    "Rock 8ths (busy kick)": drums(hats8 + [(0, K, 110), (1.5, K, 100), (2, K, 110), (2.5, K, 90), (1, S, 110), (3, S, 110)]),
    "Four on the floor":  drums([(i, K, 112) for i in range(4)] + [(1, S, 100), (3, S, 100)] + [(i + 0.5, OH, 84) for i in range(4)]),
    "Half time":          drums(hats8 + [(0, K, 110), (2.5, K, 100), (2, S, 112)]),
    "Shuffle":            drums([(i + f, H, 90 if f == 0 else 70) for i in range(4) for f in (0, 2/3)] + [(0, K, 110), (2, K, 110), (1, S, 110), (3, S, 110)]),
    "Swing ride":         drums([(i + f, R, 96 if f == 0 else 76) for i in range(4) for f in (0, 2/3)] + [(1, PH, 80), (3, PH, 80)] + [(0, K, 70), (2, K, 70)]),
    "Funk 16ths":         drums(hats16 + [(0, K, 112), (0.75, K, 96), (2, K, 112), (2.75, K, 90), (1, S, 112), (3, S, 112), (3.75, S, 70)]),
    "Bossa nova":         drums([(i * 0.5, H, 80) for i in range(8)] + [(0, K, 100), (1.5, K, 100), (2, K, 100), (3.5, K, 100)] + [(0, RIM, 90), (1.5, RIM, 90), (3, RIM, 90)]),
    "Train beat":         drums([(i * 0.25, S, 100 if i % 4 == 0 else 60 if i % 2 else 76) for i in range(16)] + [(0, K, 110), (2, K, 110)]),
    "Waltz":              drums([(0, K, 110), (1, H, 84), (2, H, 84), (1, S, 90), (2, S, 90)]),
    "6-8 ballad":         drums([(i * 0.5, H, 92 if i % 3 == 0 else 70) for i in range(6)] + [(0, K, 110), (1.5, S, 108)]),
    "Crash on one":       drums([(0, CR, 112), (0, K, 112)]),
    "Fill (toms)":        drums([(0, S, 100), (0.25, S, 100), (0.5, HT, 100), (0.75, HT, 100), (1.0, LT, 104), (1.25, LT, 104), (1.5, K, 110), (1.75, CR, 112)]),
}
here = os.path.dirname(os.path.abspath(__file__))
for folder, table, channel in (("Licks", licks, 1), ("Comping", comping, 1), ("Drums", drum_patterns, 10)):
    for name, notes in table.items():
        write(os.path.join(here, folder, name + ".mid"), notes, channel)
print(sum(len(t) for t in (licks, comping, drum_patterns)), "phrases written")
