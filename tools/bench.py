"""Run the existing cycle-exact WinUAE benchmark without replacing work/run.adf.

Windows, Python 3. Usage: python tools/bench.py --tag base --stream work/au_pcm8.a5v
Assemble a variant first and pass --player; output and decoder logs live in work/bench.
"""
import argparse
import ctypes
import json
from pathlib import Path
import struct
import subprocess
import time


ROOT = Path(__file__).resolve().parents[1]


def run(args):
    stream = Path(args.stream).resolve()
    player = Path(args.player).resolve()
    out = ROOT / 'work' / 'bench' / args.tag
    out.mkdir(parents=True, exist_ok=True)
    adf = out / (stream.stem + '.adf')
    subprocess.run([str(ROOT / 'work/mkadf.exe'), '--boot', str(ROOT / 'work/boot.bin'),
                    '--player', str(player), '--data', str(stream), '--reserve-tail', '6',
                    '--out', str(adf)], check=True, capture_output=True)
    cfg = out / (stream.stem + '.uae')
    lines = []
    for line in (ROOT / 'a500vp.uae').read_text().splitlines():
        if line.startswith('kickstart_rom_file='):
            line = 'kickstart_rom_file=' + str(Path(args.rom).resolve())
        elif line.startswith('floppy0='):
            line = 'floppy0=' + str(adf)
        lines.append(line)
    cfg.write_text('\n'.join(lines + ['use_gui=no']) + '\n', encoding='ascii')
    header = stream.read_bytes()[:32]
    frames = int.from_bytes(header[20:24], 'big')
    wait = args.wait or max(35, stream.stat().st_size / 16000 + frames * .041 + 18)
    startup = subprocess.STARTUPINFO()
    startup.dwFlags = subprocess.STARTF_USESHOWWINDOW
    startup.wShowWindow = 0
    process = subprocess.Popen([args.winuae, '-f', str(cfg)], startupinfo=startup)
    print(f'{args.tag}/{stream.stem}: WinUAE PID {process.pid}, {wait:.0f}s', flush=True)
    try:
        time.sleep(wait)
    finally:
        # Request a normal shutdown: WinUAE flushes its cached disk writes on exit.
        callback_type = ctypes.WINFUNCTYPE(ctypes.c_bool, ctypes.c_void_p, ctypes.c_void_p)
        @callback_type
        def close(hwnd, unused):
            pid = ctypes.c_ulong()
            ctypes.windll.user32.GetWindowThreadProcessId(ctypes.c_void_p(hwnd), ctypes.byref(pid))
            if pid.value == process.pid:
                ctypes.windll.user32.PostMessageW(ctypes.c_void_p(hwnd), 0x10, 0, 0)
            return True
        ctypes.windll.user32.EnumWindows(close, 0)
        try:
            process.wait(timeout=20)
        except subprocess.TimeoutExpired:
            process.kill()
            raise RuntimeError('WinUAE did not close normally; measurement is invalid')
    data = adf.read_bytes()
    info = data[1759 * 512:1760 * 512]
    if info[:4] != b'PLAY':
        raise RuntimeError(f'No PLAY measurement in {adf}; increase --wait or inspect the emulator')
    result = subprocess.run([str(ROOT / 'work/a500vp-dec.exe'), '--in', str(stream),
                             '--measure', str(adf)], capture_output=True)
    (out / (stream.stem + '.log')).write_bytes(result.stdout + result.stderr)
    result.check_returncode()
    get = lambda offset: struct.unpack_from('>I', info, offset)[0]
    clocks = struct.unpack_from('>512I', data, 1754 * 512)
    timings = [c for c in clocks[:min(frames, 512)] if c]
    summary = dict(stream=str(stream), player_bytes=player.stat().st_size,
                   load_vbl=get(8), late=get(32), max_late=get(36),
                   delta_mean_cck=sum(timings) / len(timings) if timings else 0,
                   delta_max_cck=get(56), audio_buffers=get(76),
                   audio_mean_cck=get(108) / get(76) if get(76) else 0,
                   audio_max_cck=get(112), blit_count=get(124),
                   blit_mean_cck=get(116) / get(124) if get(124) else 0,
                   blit_max_cck=get(120), fb_crc=[get(128), get(132)])
    if info[136:140] == b'AUDC':
        summary['timing_includes_audio_crc'] = True
        summary['audio_crc'] = get(140)
        summary['audio_samples_verified'] = get(144)
    (out / (stream.stem + '.json')).write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary), flush=True)


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--tag', required=True)
    parser.add_argument('--stream', required=True)
    parser.add_argument('--player', default=str(ROOT / 'work/player_bench.bin'))
    parser.add_argument('--rom', default=str(ROOT / 'kick12.rom'))
    parser.add_argument('--winuae', default=r'C:\Program Files\WinUAE\winuae64.exe')
    parser.add_argument('--wait', type=float)
    run(parser.parse_args())
