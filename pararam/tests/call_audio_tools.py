#!/usr/bin/env python3
# This file is part of Pararam Desktop,
# an alternative pararam.io client based on Telegram Desktop.
#
# For license and copyright information please follow this link:
# https://github.com/telegramdesktop/tdesktop/blob/master/LEGAL
"""Helpers of call_audio_test.sh.

tone FREQUENCY
    Writes an endless 48 kHz mono s16le sine to stdout, for pacat.
listen SERVER SECONDS
    Records the "speaker" of a PulseAudio server and prints its RMS and
    main frequency as JSON.
"""

import json
import subprocess
import sys

import numpy

RATE = 48000


def tone(frequency):
    done = 0
    while True:
        t = (numpy.arange(4800) + done) / RATE
        data = numpy.sin(2 * numpy.pi * frequency * t) * 8000
        sys.stdout.buffer.write(data.astype('<i2').tobytes())
        done += 4800


def listen(server, seconds):
    process = subprocess.run(
        ['timeout', str(seconds), 'parec', '--server=' + server,
         '-d', 'speaker.monitor', '--format=s16le', '--rate=%d' % RATE,
         '--channels=1', '--raw'],
        capture_output=True)
    data = numpy.frombuffer(process.stdout, dtype='<i2').astype(numpy.float64)
    if len(data) < RATE // 4:
        print(json.dumps({'rms': 0, 'frequency': 0}))
        return
    spectrum = numpy.abs(numpy.fft.rfft(data * numpy.hanning(len(data))))
    spectrum[0] = 0
    print(json.dumps({
        'rms': round(float(numpy.sqrt(numpy.mean(data ** 2)))),
        'frequency': round(float(numpy.argmax(spectrum) * RATE / len(data))),
    }))


if __name__ == '__main__':
    if sys.argv[1] == 'tone':
        try:
            tone(float(sys.argv[2]))
        except (BrokenPipeError, KeyboardInterrupt):
            sys.exit(0)  # The test is over.
    else:
        listen(sys.argv[2], float(sys.argv[3]))
