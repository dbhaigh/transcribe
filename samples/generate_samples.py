import math
import struct
import wave
from pathlib import Path


SAMPLE_RATE = 44100
OUTPUT = Path(__file__).resolve().parent


def write_phrase(filename, events):
    samples = []
    for frequency, duration in events:
        count = round(duration * SAMPLE_RATE)
        for index in range(count):
            time = index / SAMPLE_RATE
            edge = min(1.0, time / 0.015, (duration - time) / 0.025)
            signal = sum(
                (0.55 / harmonic) * math.sin(2.0 * math.pi * frequency * harmonic * time)
                for harmonic in range(1, 5)
            )
            samples.append(max(-1.0, min(1.0, signal * max(0.0, edge) * 0.55)))
        samples.extend([0.0] * round(0.04 * SAMPLE_RATE))

    path = OUTPUT / filename
    with wave.open(str(path), "wb") as audio:
        audio.setnchannels(1)
        audio.setsampwidth(2)
        audio.setframerate(SAMPLE_RATE)
        audio.writeframes(b"".join(
            struct.pack("<h", round(sample * 32767)) for sample in samples
        ))


write_phrase("single_a4.wav", [(440.0, 1.0)])
write_phrase(
    "guitar_scale.wav",
    [(220.00, 0.45), (246.94, 0.45), (293.66, 0.45), (329.63, 0.45), (392.00, 0.45)],
)

(OUTPUT / "single_a4.expected.txt").write_text(
    "A4 (MIDI 69), approximately 0.00-1.00 seconds\n", encoding="utf-8"
)
(OUTPUT / "guitar_scale.expected.txt").write_text(
    "A3 (57), B3 (59), D4 (62), E4 (64), G4 (67), in ascending order\n",
    encoding="utf-8",
)
