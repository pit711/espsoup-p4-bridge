#!/usr/bin/env python3
"""Flash a Waveshare ESP32-P4 module (P4 + on-module C6) in one go.

  flash.py [-p COM21]                 P4 bridge over USB, then ESPsoup onto the C6
  flash.py -p COM21 --only-p4         only the P4 bridge
  flash.py -p COM21 --only-c6         only the C6 (bridge already installed)
  flash.py -p COM21 --c6 other.bin    another C6 image, e.g. ESP-Hosted to get Wi-Fi back

Plug the cable into the USB-C socket labelled "USB" (P4 USB-Serial/JTAG,
not "USB TO UART"). Images come from ../firmware (see manifest.json there).
Needs: pip install esptool pyserial
"""
import argparse, json, os, subprocess, sys, time, zlib

import serial

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import p4tool  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FW = os.path.join(ROOT, 'firmware')


def flash_p4(port, manifest):
    args = [sys.executable, '-m', 'esptool', '--chip', 'esp32p4', '-p', port, '-b', '921600',
            '--before', 'default-reset', '--after', 'hard-reset', 'write-flash']
    for part in manifest['p4']:
        args += [part['offset'], os.path.join(FW, part['file'])]
    print('>', ' '.join(args))
    return subprocess.call(args) == 0


def open_link(port, seconds=20):
    """The P4 re-enumerates after a reset; retry until the port is back."""
    t = time.time()
    while time.time() - t < seconds:
        try:
            return p4tool.Link(port=port)
        except (serial.SerialException, OSError):
            time.sleep(0.5)
    raise SystemExit(f'{port} did not come back')


def wait_mode(port, want, seconds=60):
    """Poll "P4 INFO" until the bridge has finished probing the C6."""
    t = time.time()
    l = None
    while time.time() - t < seconds:
        try:
            if l is None:
                l = open_link(port)
            l.drain(0.2)
            l.line('P4 INFO')
            r = l.readline(2)
            while r is not None and not r.startswith('P4 mode='):
                r = l.readline(2)
            if r:
                print(r)
                mode = r.split()[1].split('=')[1]
                if mode in want:
                    return l, mode
        except (serial.SerialException, OSError):
            l = None                       # bridge restarted (it leaves ESP-Hosted by rebooting)
        time.sleep(1)
    raise SystemExit('bridge did not report ' + '/'.join(want))


def flash_c6(port, path):
    l, mode = wait_mode(port, ('bridge', 'hosted'))
    data = open(path, 'rb').read()
    crc = zlib.crc32(data) & 0xffffffff
    print(f'C6 runs {mode}; sending {os.path.basename(path)} ({len(data)} bytes)')
    l.drain(0.2)
    l.line(f'P4 OTA {len(data)} {crc:08x}')
    if not p4tool.upload(l, data):
        return False
    while True:
        try:
            r = l.readline(30)
        except (serial.SerialException, OSError):
            break                          # restart on the way out of ESP-Hosted
        if r is None or r.startswith('P4 MODE'):
            break
        print(r)
        if r.startswith('P4 ERR'):
            return False
    try:
        l.s.close()
    except Exception:
        pass
    l, mode = wait_mode(port, ('bridge', 'hosted'))
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('-p', '--port', default='COM21')
    ap.add_argument('--only-p4', action='store_true')
    ap.add_argument('--only-c6', action='store_true')
    ap.add_argument('--c6', help='C6 image instead of the one in firmware/manifest.json')
    a = ap.parse_args()
    manifest = json.load(open(os.path.join(FW, 'manifest.json')))
    if not a.only_c6:
        if not flash_p4(a.port, manifest):
            return 1
        time.sleep(2)
    if not a.only_p4:
        if not flash_c6(a.port, a.c6 or os.path.join(FW, manifest['c6']['file'])):
            return 1
    print('done')
    return 0


if __name__ == '__main__':
    sys.exit(main())
