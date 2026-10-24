#!/usr/bin/env python3
"""Talk to the ESP32-P4 bridge (USB-Serial/JTAG, e.g. COM21) or its TCP port.

  p4tool.py [-p COM21 | -t 192.168.x.y] info
  p4tool.py ... cmd "INFO"              send one line to the C6, print replies
  p4tool.py ... ota firmware.bin        flash the C6 through the P4
  p4tool.py ... update p4bridge.bin     flash the P4 bridge itself (restarts)
  p4tool.py ... www build/www.bin       upload the web app image (LittleFS) to the www partition
  p4tool.py ... reset
  p4tool.py ... bench [seconds] [depth] CAP throughput; depth 1 = one round trip per frame, 2 = next command queued early
  p4tool.py ... stream [seconds]        RXRUN throughput, frames back to back
  p4tool.py ... log [seconds]
"""
import argparse, socket, struct, sys, time, zlib

import serial


class Link:
    def __init__(self, port=None, host=None):
        if host:
            self.s = socket.create_connection((host, 5555), timeout=5)
            self.s.settimeout(0.2)
            self.kind = 'tcp'
        else:
            # USB-Serial/JTAG resets the chip on DTR/RTS: keep both low.
            self.s = serial.Serial()
            self.s.port, self.s.baudrate, self.s.timeout = port, 2000000, 0.2
            self.s.dtr = False
            self.s.rts = False
            self.s.open()
            self.kind = 'ser'

    def write(self, b):
        if self.kind == 'tcp':
            self.s.sendall(b)
        else:
            self.s.write(b)
            self.s.flush()

    def read(self, n):
        try:
            return self.s.recv(n) if self.kind == 'tcp' else self.s.read(n)
        except socket.timeout:
            return b''

    def line(self, s):
        self.write(s.encode() + b'\n')

    def readline(self, timeout=2.0):
        buf = b''
        t = time.time()
        while time.time() - t < timeout:
            c = self.read(1)
            if not c:
                continue
            if c == b'\n':
                return buf.decode('latin1')
            buf += c
        return None

    def drain(self, timeout=0.3):
        out = b''
        t = time.time()
        while time.time() - t < timeout:
            c = self.read(4096)
            if c:
                out += c
                t = time.time()
        return out


def cmd_info(l):
    l.line('P4 INFO')
    t = time.time()
    while time.time() - t < 4:
        r = l.readline(1)
        if r is None:
            continue
        print(r)
        if r.startswith('P4 '):
            return
    print('(no reply)')


def cmd_log(l, seconds):
    t = time.time()
    while time.time() - t < seconds:
        r = l.readline(1)
        if r is not None:
            print(r)


def cmd_cmd(l, text):
    l.drain(0.1)
    l.line(text)
    sys.stdout.write(l.drain(1.0).decode('latin1', 'replace'))


def cmd_reset(l):
    l.line('P4 RESET')
    for _ in range(10):
        r = l.readline(8)
        if r is None:
            break
        print(r)
        if r.startswith('P4 MODE'):
            break


def upload(l, data):
    """After a P4 OTA/UPDATE line: wait for P4 READY, then send one chunk per
    P4 ACK (stop-and-wait, the USB receive buffer must never overflow)."""
    while True:
        r = l.readline(10)
        print(r)
        if r is None or r.startswith('P4 ERR'):
            return False
        if r.startswith('P4 READY'):
            break
    chunk = int(r.split()[2]) if len(r.split()) > 2 else 1400
    t0 = time.time()
    last = time.time()
    for off in range(0, len(data), chunk):
        l.write(data[off:off + chunk])
        while True:
            r = l.readline(10)
            if r is None:
                print('(no ACK)')
                return False
            if r.startswith('P4 ACK'):
                break
            print(r)
        if time.time() - last > 2:
            last = time.time()
            print(f'{off + chunk}/{len(data)} {(off + chunk) / (time.time() - t0) / 1e3:.0f} kB/s')
    print(f'{len(data)} bytes in {time.time() - t0:.1f} s')
    return True


def cmd_ota(l, path):
    data = open(path, 'rb').read()
    crc = zlib.crc32(data) & 0xffffffff
    l.drain(0.2)
    l.line(f'P4 OTA {len(data)} {crc:08x}')
    if not upload(l, data):
        return 1
    while True:
        r = l.readline(60)
        if r is None:
            print('(timeout)')
            return 1
        print(r)
        if r.startswith('P4 MODE'):
            return 0
        if r.startswith('P4 ERR'):
            return 1


def cmd_update(l, path):
    data = open(path, 'rb').read()
    crc = zlib.crc32(data) & 0xffffffff
    l.drain(0.2)
    l.line(f'P4 UPDATE {len(data)} {crc:08x}')
    if not upload(l, data):
        return 1
    while True:
        r = l.readline(30)
        if r is None:
            print('(timeout)')
            return 1
        print(r)
        if r.startswith('P4 OK update'):
            print('bridge restarts; run "info" in a few seconds')
            return 0
        if r.startswith('P4 ERR'):
            return 1


def cmd_www(l, path):
    """Upload the LittleFS image of the web app (build/www.bin) into the www partition."""
    data = open(path, 'rb').read()
    end = len(data.rstrip(b'\xff'))          # the 0xFF tail is erased by the P4 itself
    data = data[:max(8192, (end + 4095) & ~4095)]
    crc = zlib.crc32(data) & 0xffffffff
    l.drain(0.2)
    l.line(f'P4 WWW {len(data)} {crc:08x}')
    if not upload(l, data):
        return 1
    while True:
        r = l.readline(60)
        if r is None:
            print('(timeout)')
            return 1
        print(r)
        if r.startswith('P4 OK www'):
            return 0
        if r.startswith('P4 ERR'):
            return 1


def read_frame(l, n):
    """One DATA header plus n*2 payload bytes; returns (header, bytes) or (header, -1)."""
    hdr = l.readline(3)
    if not hdr or not hdr.startswith('DATA'):
        return hdr, -1
    want = n * 2
    got = 0
    t = time.time()
    while got < want and time.time() - t < 3:
        got += len(l.read(want - got))
    return hdr, got if got == want else -1


def start_rx(l):
    l.drain(0.2)
    l.line('INFO')
    print('INFO ->', l.readline())
    l.line('FREQ 2437')
    print('FREQ ->', l.readline())


def cmd_bench(l, seconds, depth=1):
    """CAP16 frames; `depth` commands are kept in flight (1 = strict
    stop-and-wait, 2 = the next capture is already queued while a frame
    is still on its way)."""
    start_rx(l)
    n = 16380
    total = 0
    frames = 0
    pending = 0
    t0 = time.time()
    while time.time() - t0 < seconds or pending:
        while pending < depth and time.time() - t0 < seconds:
            l.line(f'CAP16 {n} 0')
            pending += 1
        hdr, got = read_frame(l, n)
        pending -= 1
        if got < 0:
            print('bad frame', hdr)
            break
        total += got + len(hdr) + 1
        frames += 1
    dt = time.time() - t0
    print(f'{frames} frames, {total / 1e6:.2f} MB in {dt:.1f} s = {total / dt / 1e3:.0f} kB/s (depth {depth})')


def cmd_stream(l, seconds):
    """RXRUN streams frames without a PC round trip in between."""
    start_rx(l)
    n = 16380
    repeats = min(1000, max(10, int(seconds * 100)))
    l.line(f'RXRUN {n} 0 {repeats} 16')
    total = 0
    frames = 0
    t0 = time.time()
    while True:
        hdr, got = read_frame(l, n)
        if hdr is not None and hdr.startswith('END'):
            break
        if got < 0:
            print('bad frame', hdr)
            break
        total += got + len(hdr) + 1
        frames += 1
    dt = time.time() - t0
    print(f'{frames} frames, {total / 1e6:.2f} MB in {dt:.1f} s = {total / dt / 1e3:.0f} kB/s')


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('-p', '--port', default='COM21')
    ap.add_argument('-t', '--tcp', help='bridge IP (TCP 5555) instead of serial')
    ap.add_argument('what', nargs='+')
    a = ap.parse_args()
    l = Link(None if a.tcp else a.port, a.tcp)
    w = a.what
    if w[0] == 'info':
        cmd_info(l)
    elif w[0] == 'cmd':
        cmd_cmd(l, ' '.join(w[1:]))
    elif w[0] == 'reset':
        cmd_reset(l)
    elif w[0] == 'ota':
        sys.exit(cmd_ota(l, w[1]))
    elif w[0] == 'update':
        sys.exit(cmd_update(l, w[1]))
    elif w[0] == 'www':
        sys.exit(cmd_www(l, w[1]))
    elif w[0] == 'bench':
        cmd_bench(l, float(w[1]) if len(w) > 1 else 10, int(w[2]) if len(w) > 2 else 1)
    elif w[0] == 'stream':
        cmd_stream(l, float(w[1]) if len(w) > 1 else 10)
    elif w[0] == 'log':
        cmd_log(l, float(w[1]) if len(w) > 1 else 10)
    else:
        ap.error('unknown command')


if __name__ == '__main__':
    main()
