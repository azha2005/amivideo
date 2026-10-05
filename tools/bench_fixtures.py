"""Deterministic v6 streams: dense/sparse rows, copies, repeats and PCM boundaries.

python tools/bench_fixtures.py writes independent frame/audio CRCs in work/bench/fixtures.
"""
from pathlib import Path
import random
import struct
import zlib

ROOT = Path(__file__).resolve().parents[1]
OUT = ROOT / 'work/bench/fixtures'


def fixture(planes, pcm=False):
    rng = random.Random(68000 + planes)
    frames = 72
    palette = b''.join(struct.pack('>H', c * 0x111 % 4096) for c in range(1 << planes))
    buffers = [bytearray(planes * 128 * 20) for _ in range(2)]
    visible = 1
    packets, crcs, audio = [], [], bytearray()
    # Zero, small tails, exact MOVEM blocks, a buffer split, and packet splits.
    lengths = [0, 2, 14, 16, 18, 30, 32, 34, 510, 512, 514, 1600]
    for frame in range(frames):
        flags = 1 if frame == 0 else 0
        is_repeat = frame in (23, 47)
        chunk = bytes(rng.randrange(256) for _ in range(lengths[frame % len(lengths)])) if pcm else b''
        audio.extend(chunk)
        video = bytearray()
        if not is_repeat:
            hidden = visible ^ 1
            if frame % 3 == 2:
                flags |= 2
                buffers[hidden][:] = buffers[visible]
            rowmask = bytearray(16)
            rows = bytearray()
            for y in range(16, 112):
                if frame < 24:
                    cols = list(range(20))
                elif frame < 48:
                    patterns = [[0], [19], list(range(8)), list(range(8, 16)),
                                list(range(16, 20)), list(range(19)), [0, 8, 19]]
                    cols = patterns[(frame + y) % len(patterns)]
                else:
                    cols = list(range(20)) if y % 2 else [0, 7, 8, 15, 16, 19]
                rowmask[y // 8] |= 128 >> (y % 8)
                mask = sum(1 << (23 - c) for c in cols)
                rows.extend(mask.to_bytes(3, 'big'))
                for c in cols:
                    for p in range(planes):
                        value = rng.randrange(256)
                        rows.append(value)
                        buffers[hidden][(p * 128 + y) * 20 + c] = value
            video = rowmask + rows
            visible = hidden
        body = (palette if flags & 1 else b'') + chunk + video
        size = (6 + len(body) + 1) & ~1
        packets.append(struct.pack('>HBBH', size, int(is_repeat), flags, len(chunk)) +
                       body + bytes(size - 6 - len(body)))
        # Independent bitplane -> pixel conversion for the encoder sidecar CRC.
        pixels = bytearray(160 * 128)
        for p in range(planes):
            for y in range(128):
                for c in range(20):
                    value = buffers[visible][(p * 128 + y) * 20 + c]
                    for bit in range(8):
                        if value & (128 >> bit):
                            pixels[y * 160 + c * 8 + bit] |= 1 << p
        crcs.append(struct.pack('>I', zlib.crc32(palette, zlib.crc32(pixels))))
    payload = b''.join(packets)
    header = struct.pack('>4sHHHHBBHHHII4B', b'A5VP', 6, int(pcm), 160, 128,
                         planes, 1 << planes, 443 if pcm else 0, 16, 112,
                         frames, len(payload), 2 if pcm else 0, 0, 0, 0)
    path = OUT / (f'p{planes}' + ('_pcm_edges' if pcm else '') + '.a5v')
    path.write_bytes(header + payload)
    path.with_suffix('.a5v.crc').write_bytes(b''.join(crcs) +
                                           (struct.pack('>I', zlib.crc32(audio)) if pcm else b''))
    print(path, len(header + payload))


if __name__ == '__main__':
    OUT.mkdir(parents=True, exist_ok=True)
    for planes in (2, 3, 4, 5):
        fixture(planes)
    fixture(3, pcm=True)
