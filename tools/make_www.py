#!/usr/bin/env python3
"""Stage the ESPsoup web app for the P4 bridge's "www" partition.

  make_www.py <staging-dir> [-o <out-dir>]     (default out: <project>/www)

Takes the directory with the finished web app (index.html at its top) and
fills the output directory with what the bridge serves:

  * compressible files (html js mjs css json svg txt md csv map webmanifest)
    are gzipped as <name>.gz, the original is NOT copied; the bridge serves
    <path>.gz with "Content-Encoding: gzip" when a browser asks for <path>
  * everything else (png, wasm, woff2, ...) is copied unchanged

The output directory is emptied first. The LittleFS image (build/www.bin)
is then produced by "idf.py build" from the output directory
(littlefs_create_partition_image in main/CMakeLists.txt). Standard library
only, no pip packages.

Checks: every path component must fit LittleFS' name limit
(CONFIG_LITTLEFS_OBJ_NAME_LEN 64 incl. NUL), the stored total must fit the
partition with some headroom for LittleFS metadata. Exit code 1 on a problem.
"""
import argparse
import gzip
import os
import shutil
import sys

GZIP_EXT = {'html', 'js', 'mjs', 'css', 'json', 'svg', 'txt', 'md', 'csv', 'map', 'webmanifest'}
NAME_MAX = 63                 # CONFIG_LITTLEFS_OBJ_NAME_LEN - 1
PARTITION = 0x600000          # "www" in partitions.csv
BLOCK = 4096                  # LittleFS block size; every file costs whole blocks
HEADROOM = 32 * BLOCK         # superblocks + directory metadata


def stored_size(n):
    """Flash blocks a file of n bytes occupies (CTZ list: 4 or 8 pointer bytes per block)."""
    blocks = 0
    while n > 0:
        blocks += 1
        n -= BLOCK - (8 if blocks > 1 else 0)
    return max(blocks, 1) * BLOCK


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('staging', help='directory with the built web app')
    ap.add_argument('-o', '--out', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'www'),
                    help='output directory (default: <project>/www)')
    ap.add_argument('-q', '--quiet', action='store_true')
    a = ap.parse_args()

    src = os.path.abspath(a.staging)
    out = os.path.abspath(a.out)
    if not os.path.isdir(src):
        sys.exit(f'{src}: not a directory')
    try:
        nested = os.path.commonpath([src, out]) == out
    except ValueError:          # different drives on Windows: cannot be nested
        nested = False
    if nested:
        sys.exit('output directory must not contain the staging directory')
    if not os.path.isfile(os.path.join(src, 'index.html')):
        print('warning: no index.html at the top of the staging directory', file=sys.stderr)

    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(out)

    problems = 0
    files = 0
    raw_total = stored_total = flash_total = 0
    for root, dirs, names in os.walk(src):
        dirs.sort()
        rel_dir = os.path.relpath(root, src)
        rel_dir = '' if rel_dir == '.' else rel_dir.replace(os.sep, '/')
        for d in dirs:
            if len(d) > NAME_MAX:
                print(f'ERROR: directory name too long ({len(d)} > {NAME_MAX}): {rel_dir}/{d}', file=sys.stderr)
                problems += 1
        if rel_dir:
            os.makedirs(os.path.join(out, rel_dir), exist_ok=True)
        for name in sorted(names):
            path = os.path.join(root, name)
            rel = f'{rel_dir}/{name}' if rel_dir else name
            ext = name.rsplit('.', 1)[-1].lower() if '.' in name else ''
            data = open(path, 'rb').read()
            raw_total += len(data)
            if ext in GZIP_EXT:
                out_name = name + '.gz'
                stored = gzip.compress(data, compresslevel=9, mtime=0)
            else:
                out_name = name
                stored = data
            if len(out_name) > NAME_MAX:
                print(f'ERROR: file name too long ({len(out_name)} > {NAME_MAX}): {rel}', file=sys.stderr)
                problems += 1
            with open(os.path.join(out, rel_dir, out_name), 'wb') as f:
                f.write(stored)
            files += 1
            stored_total += len(stored)
            flash_total += stored_size(len(stored))
            if not a.quiet:
                tag = f'gz {len(stored) * 100 // max(len(data), 1):3d}%' if ext in GZIP_EXT else 'copy   '
                print(f'{len(data):9d} -> {len(stored):9d}  {tag}  {rel}')

    print(f'{files} files, {raw_total / 1e6:.2f} MB raw, {stored_total / 1e6:.2f} MB stored, '
          f'~{flash_total / 1e6:.2f} MB in flash blocks of {PARTITION / 1e6:.0f} MB -> {out}')
    if flash_total + HEADROOM > PARTITION:
        print(f'ERROR: does not fit the {PARTITION / 1e6:.0f} MB www partition', file=sys.stderr)
        problems += 1
    if problems:
        sys.exit(1)


if __name__ == '__main__':
    main()
