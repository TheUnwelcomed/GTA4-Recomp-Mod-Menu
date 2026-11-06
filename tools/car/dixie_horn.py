"""The General Lee's Dixie horn, synthesized (v1.7.47): the twelve notes of
the opening of "Dixie" (1859) as a five-trumpet air horn plays them.

Nothing sampled. The notes and their pace were measured on the user's
livery pack's clip (pitch tracked at 10 ms): mi re do do do re mi fa sol
sol sol mi in B (F#5 D#5 B4 B4 B4 C#5 D#5 E5 F#5 F#5 F#5 D#5), the short
notes about 0.12 s, the repeated ones about 0.26 s, the whole 2.6 s.

An air-horn trumpet (v3, "a horn, not a ringtone"): a diaphragm snapping
on compressed air, so a narrow pulse train (not a sine stack) with a
cycle-to-cycle jitter and a 40 Hz chatter in its amplitude; the pitch
scooping up over the first 40 ms as the air pressure builds and sagging as
the valve shuts; the pulses through the trumpet's bell, three resonances
(about 900, 1700 and 2600 Hz) that give it the brassy nasal blare; a blast
of turbulent air with each note and a breath of it under the tone; a soft
clip (the diaphragm's own); five trumpets a few cents apart. Then the
compressor and the limiter a horn amp has, and the long-term spectrum
pulled towards the pack clip's third-octave bands (--match), so the same
bands ring - nothing of the clip copied.

usage: python3 -I dixie_horn.py <out.wav> [semitone shift] [--match=<clip.wav>]
The same recipe runs in the trainer (src/gta4_horn_sound.h) on the horn.
"""
import math
import sys
import wave

import numpy as np
from scipy import signal

RATE = 44100
# (note name, seconds)
MELODY = [('F#5', 0.12), ('D#5', 0.12), ('B4', 0.26), ('B4', 0.26), ('B4', 0.26), ('C#5', 0.17), ('D#5', 0.12),
          ('E5', 0.14), ('F#5', 0.27), ('F#5', 0.27), ('F#5', 0.27), ('D#5', 0.30)]
GAP = 0.012  # the valve's change between notes
NAMES = ['C', 'C#', 'D', 'D#', 'E', 'F', 'F#', 'G', 'G#', 'A', 'A#', 'B']
BELL = [(900.0, 6.0, 1.0), (1700.0, 8.0, 1.3), (2600.0, 9.0, 0.8)]  # (Hz, Q, gain)


def freq(name, shift=0):
    k = NAMES.index(name[:-1]) + 12 * (int(name[-1]) + 1)
    return 440.0 * 2 ** ((k - 69 + shift) / 12.0)


def diaphragm(f, length, dur, rng):
    """One trumpet's diaphragm: a narrow pulse train at the note, jittered,
    the pitch scooping up at the start and sagging at the end, a 40 Hz
    chatter in its level."""
    t = np.arange(length) / RATE
    scoop = 1.0 - 0.09 * np.exp(-t / 0.035)
    sag = np.where(t > dur, 1.0 - 0.04 * np.minimum((t - dur) / 0.06, 1.0), 1.0)
    inst = f * scoop * sag * (1.0 + 0.004 * np.sin(2 * math.pi * 5.3 * t + rng.uniform(0, 6)))
    # per-cycle jitter: the phase advanced with a little noise on each step
    jitter = 1.0 + 0.012 * signal.lfilter([1.0], [1.0, -0.9], rng.standard_normal(length)) * 0.3
    phase = np.cumsum(inst * jitter) / RATE + rng.uniform(0, 1)
    frac = phase - np.floor(phase)
    duty = 0.18
    pulse = np.where(frac < duty, 1.0, -duty / (1.0 - duty))  # (zero-mean pulse)
    # the pulse's edges softened a little (the diaphragm is not a switch)
    pulse = signal.lfilter([0.5, 0.5], [1.0], pulse)
    chatter = 1.0 + 0.10 * np.sin(2 * math.pi * 41.0 * t + rng.uniform(0, 6)) + 0.05 * np.sin(2 * math.pi * 63.0 * t)
    return pulse * chatter


def bell(x):
    """The trumpet's horn: three resonances (peaking filters), then a tilt."""
    y = x
    for fc, q, gain in BELL:
        b, a = signal.iirpeak(fc, q, fs=RATE)
        y = y + gain * signal.lfilter(b, a, x)
    # a highpass at 300 (the horn carries no bass) and a gentle rolloff above 7 kHz
    b, a = signal.butter(2, 300.0, 'highpass', fs=RATE)
    y = signal.lfilter(b, a, y)
    b, a = signal.butter(2, 7000.0, 'lowpass', fs=RATE)
    return signal.lfilter(b, a, y)


def band_envelope(y, rate, bands):
    spec = np.abs(np.fft.rfft(y)) ** 2
    f = np.fft.rfftfreq(len(y), 1 / rate)
    out = []
    for c in bands:
        lo, hi = c / 2 ** (1 / 6), c * 2 ** (1 / 6)
        sel = (f >= lo) & (f < hi)
        out.append(np.sqrt(spec[sel].mean()) if sel.any() else 0.0)
    return np.array(out)


def match_spectrum(y, rate, clip_path, strength=0.6):
    """y with its third-octave levels pulled towards the clip's (in dB, by
    `strength`), a smooth EQ."""
    with wave.open(clip_path, 'rb') as w:
        c = np.frombuffer(w.readframes(w.getnframes()), '<i2').astype(np.float64) / 32768
        crate = w.getframerate()
        if w.getnchannels() > 1:
            c = c.reshape(-1, w.getnchannels()).mean(1)
    bands = 250.0 * 2 ** (np.arange(0, 16) / 3.0)   # 250 Hz .. 8 kHz
    a = band_envelope(y, rate, bands)
    b = band_envelope(c, crate, bands)
    a /= a.max() + 1e-12
    b /= b.max() + 1e-12
    gain_db = np.clip(strength * (20 * np.log10(b + 1e-4) - 20 * np.log10(a + 1e-4)), -10, 10)
    spec = np.fft.rfft(y)
    f = np.fft.rfftfreq(len(y), 1 / rate)
    g = np.interp(np.log(np.maximum(f, 1.0)), np.log(bands), gain_db, left=gain_db[0], right=gain_db[-1])
    return np.fft.irfft(spec * 10 ** (g / 20.0), len(y))


def compress(mix):
    """A fast follower (2 ms up, 80 ms down), 4:1 over a low knee, then a
    hard limiter: every note up front at one loudness, as a horn amp has it."""
    mix = mix / (np.abs(mix).max() + 1e-9)
    a_up, a_dn = math.exp(-1.0 / (0.002 * RATE)), math.exp(-1.0 / (0.08 * RATE))
    ax = np.abs(mix)
    env = np.zeros(len(mix))
    e = 0.0
    for i in range(len(mix)):
        e = ax[i] + (a_up if ax[i] > e else a_dn) * (e - ax[i])
        env[i] = e
    thresh, ratio = 0.18, 4.0
    gain = np.where(env > thresh, (env / thresh) ** (1.0 / ratio - 1.0), 1.0)
    mix = mix * gain * 2.6
    return np.tanh(mix * 1.6) / math.tanh(1.6)


def render(shift=0, seed=3, match=None):
    rng = np.random.default_rng(seed)
    total = sum(d for _, d in MELODY) + GAP * len(MELODY) + 0.15
    n = int(total * RATE)
    tone_mix = np.zeros(n)
    air_mix = np.zeros(n)
    at = 0.0
    for name, dur in MELODY:
        f = freq(name, shift)
        start = int(at * RATE)
        length = min(int((dur + 0.08) * RATE), n - start)
        tt = np.arange(length) / RATE
        env = np.minimum(1.0, tt / 0.006) * np.where(tt < dur, 1.0, np.exp(-(tt - dur) / 0.035))
        tone = np.zeros(length)
        for k in range(5):  # five trumpets a few cents apart, beating
            tone += diaphragm(f * 2 ** (rng.uniform(-7, 7) / 1200.0), length, dur, rng)
        tone /= 5.0
        tone = np.tanh(tone * 1.8)  # the diaphragm's own clip
        # the air: a blast as the valve opens, a breath under the note, both
        # modulated by the pulses (the air comes in puffs)
        puff = 0.6 + 0.4 * np.clip(tone, 0, 1)
        blast = rng.standard_normal(length) * np.exp(-tt / 0.03) * 1.6
        breath = rng.standard_normal(length) * 0.16 * puff
        tone_mix[start:start + length] += tone * env
        air_mix[start:start + length] += (blast + breath) * env
        at += dur + GAP
    # the air through a band about 3 kHz (turbulence at the mouth), the tone through the bell
    b, a = signal.butter(2, [1800.0, 6500.0], 'bandpass', fs=RATE)
    air = signal.lfilter(b, a, air_mix)
    mix = bell(tone_mix) + air * 0.9
    if match:
        mix = match_spectrum(mix, RATE, match)
    mix = compress(mix)
    return (mix * 0.97).astype(np.float32)


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    match = next((a[len('--match='):] for a in sys.argv[1:] if a.startswith('--match=')), None)
    out = args[0]
    shift = float(args[1]) if len(args) > 1 else 0.0
    y = render(shift, match=match)
    with wave.open(out, 'wb') as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(RATE)
        w.writeframes(np.clip(y * 32767, -32768, 32767).astype('<i2').tobytes())
    print('%s: %.2f s, %d notes' % (out, len(y) / RATE, len(MELODY)))


if __name__ == '__main__':
    main()
