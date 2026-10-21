"""Seeds for the fuzz targets (fuzz/corpus/<target>/): small valid files the fuzzer starts from.

  python fuzz/make_seeds.py     (numpy, soundfile; ffmpeg on PATH for the MP3 seed, skipped without it)

dsd:     a stereo DSF (one block of 4096 bytes per channel), the same with 1-bit LSB-first and 8-bit MSB-first data,
         a stereo DFF and a 5.1 DFF (CHNL ids)
decoder: the first byte picks the type (fuzz_decoder.cpp: 0 wav, 1 aiff, 2 flac, 3 mp3), the rest is the file
"""
import io
import os
import shutil
import struct
import subprocess
import sys
import tempfile

import numpy as np
import soundfile as sf

HERE = os.path.dirname(os.path.abspath(__file__))


def out(target, name, data):
    d = os.path.join(HERE, 'corpus', target)
    os.makedirs(d, exist_ok=True)
    with open(os.path.join(d, name), 'wb') as f:
        f.write(data)
    print(f'{target}/{name}: {len(data)} bytes')


def dsd_bytes(n, seed):
    """n bytes of a plausible DSD stream (a 1-bit sigma-delta of a quiet sine)"""
    t = np.arange(n * 8)
    x = 0.3 * np.sin(2 * np.pi * 1000 * t / 2822400 + seed)
    bits, acc = np.empty(n * 8, np.uint8), 0.0
    for i, v in enumerate(x):
        b = 1 if acc < v else 0
        acc += (1 if b else -1) - v
        bits[i] = b
    return np.packbits(bits).tobytes()   # MSB first


def dsf(ch, bps, frames=4096):
    block = 4096
    data = b''
    for c in range(ch):
        d = dsd_bytes(frames, c)
        if bps == 1:   # LSB first
            d = bytes(int(f'{v:08b}'[::-1], 2) for v in d)
        data += d + bytes(block - frames)
    fmt = b'fmt ' + struct.pack('<QIIIIIIQII', 52, 1, 0, 2 if ch == 2 else 1, ch, 2822400, bps, frames * 8, block, 0)
    body = b'data' + struct.pack('<Q', 12 + len(data)) + data
    total = 28 + len(fmt) + len(body)
    return b'DSD ' + struct.pack('<QQQ', 28, total, 0) + fmt + body


def dff(ids, frames=1000):
    ch = len(ids)
    snd = bytearray()
    streams = [dsd_bytes(frames, c) for c in range(ch)]
    for i in range(frames):
        for c in range(ch):
            snd.append(streams[c][i])

    def chunk(cid, body):
        return cid + struct.pack('>Q', len(body)) + body + (b'\0' if len(body) & 1 else b'')

    chnl = struct.pack('>H', ch) + b''.join(ids)
    prop = b'SND ' + chunk(b'FS  ', struct.pack('>I', 2822400)) + chunk(b'CHNL', chnl) + chunk(b'CMPR', b'DSD \x0eNot Compressed\x00')
    form = b'DSD ' + chunk(b'FVER', struct.pack('>I', 0x01050000)) + chunk(b'PROP', prop) + chunk(b'DSD ', bytes(snd))
    return b'FRM8' + struct.pack('>Q', len(form)) + form


def pcm(frames=256, ch=2):
    t = np.arange(frames) / 44100
    return np.stack([0.5 * np.sin(2 * np.pi * (440 + 220 * c) * t) for c in range(ch)], axis=1)


def sound(fmt, subtype):
    b = io.BytesIO()
    sf.write(b, pcm(), 44100, format=fmt, subtype=subtype)
    return b.getvalue()


def mp3():
    ff = shutil.which('ffmpeg')
    if not ff:
        print('decoder/mp3: no ffmpeg on PATH, skipped')
        return None
    with tempfile.TemporaryDirectory() as d:
        w, m = os.path.join(d, 'a.wav'), os.path.join(d, 'a.mp3')
        sf.write(w, pcm(4608), 44100, subtype='PCM_16')
        subprocess.run([ff, '-v', 'error', '-y', '-i', w, '-b:a', '32k', '-map_metadata', '-1', '-id3v2_version', '0',
                        '-write_xing', '0', m], check=True)
        return open(m, 'rb').read()


def main():
    out('dsd', 'stereo_lsb.dsf', dsf(2, 1))
    out('dsd', 'stereo_msb.dsf', dsf(2, 8))
    out('dsd', 'mono_short.dsf', dsf(1, 1, frames=1000))
    out('dsd', 'stereo.dff', dff([b'SLFT', b'SRGT']))
    out('dsd', 'surround.dff', dff([b'MLFT', b'MRGT', b'C   ', b'LFE ', b'LS  ', b'RS  ']))
    out('decoder', 'wav_pcm16', b'\0' + sound('WAV', 'PCM_16'))
    out('decoder', 'wav_float', b'\0' + sound('WAV', 'FLOAT'))
    out('decoder', 'wav_pcm24', b'\0' + sound('WAV', 'PCM_24'))
    out('decoder', 'aiff_pcm16', b'\1' + sound('AIFF', 'PCM_16'))
    out('decoder', 'flac_16', b'\2' + sound('FLAC', 'PCM_16'))
    out('decoder', 'flac_24', b'\2' + sound('FLAC', 'PCM_24'))
    m = mp3()
    if m:
        out('decoder', 'mp3_32k', b'\3' + m)


if __name__ == '__main__':
    sys.exit(main())
