#!/usr/bin/env python3
# SPDX-FileCopyrightText: 2026 admiraly
# SPDX-License-Identifier: LicenseRef-PolyForm-Perimeter-1.0.1
"""Generate tests/fixtures/audio/tone440_stereo.ogg: a synthetic test signal for the Ogg Vorbis decoder test.

0.5 s at 44100 Hz, stereo: a 440 Hz sine at amplitude 0.5 on the left and 0.25 on the right. The signal is computed
here; libsndfile (through the python-soundfile package) only encodes it to Ogg Vorbis. Encoding is not
bit-reproducible across libsndfile/libvorbis versions, so the test checks length, channel levels and frequency,
not bytes. Recorded generator environment: python-soundfile 0.14.0, libsndfile 1.2.2.

Needs: pip install soundfile numpy (in a throwaway environment; not a project dependency).
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "tests", "fixtures", "audio", "tone440_stereo.ogg")


def main():
    import numpy as np
    import soundfile as sf

    rate = 44100
    t = np.arange(rate // 2) / rate
    s = np.sin(2 * np.pi * 440.0 * t)
    data = np.stack([0.5 * s, 0.25 * s], axis=1).astype(np.float32)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    sf.write(OUT, data, rate, format="OGG", subtype="VORBIS")
    print("wrote %s (%d bytes) with soundfile %s, libsndfile %s" % (os.path.relpath(OUT, ROOT), os.path.getsize(OUT),
                                                                     sf.__version__, sf.__libsndfile_version__))
    return 0


if __name__ == "__main__":
    sys.exit(main())
