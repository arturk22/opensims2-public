#!/usr/bin/env python3
"""Convert a CloneCD MODE1/2352 raw image (.img) into a 2048-byte/sector ISO."""
import argparse
import sys

RAW_SECTOR = 2352
USER_DATA_OFFSET = 16
USER_DATA_SIZE = 2048
SYNC = b"\x00" + b"\xff" * 10 + b"\x00"
CHUNK_SECTORS = 4096


def convert(src, dst):
    with open(src, "rb") as fin, open(dst, "wb") as fout:
        fin.seek(0, 2)
        size = fin.tell()
        fin.seek(0)
        if size % RAW_SECTOR:
            sys.exit(f"{src}: size {size} is not a multiple of {RAW_SECTOR}")
        total = size // RAW_SECTOR
        done = 0
        while done < total:
            buf = fin.read(RAW_SECTOR * min(CHUNK_SECTORS, total - done))
            for i in range(0, len(buf), RAW_SECTOR):
                sector = buf[i:i + RAW_SECTOR]
                if sector[:12] != SYNC:
                    sys.exit(f"{src}: bad sync at sector {done + i // RAW_SECTOR}")
                if sector[15] != 1:
                    sys.exit(f"{src}: sector {done + i // RAW_SECTOR} is mode {sector[15]}, expected 1")
                fout.write(sector[USER_DATA_OFFSET:USER_DATA_OFFSET + USER_DATA_SIZE])
            done += len(buf) // RAW_SECTOR
    with open(dst, "rb") as f:
        f.seek(16 * USER_DATA_SIZE)
        pvd = f.read(6)
    if pvd[1:6] != b"CD001":
        sys.exit(f"{dst}: no ISO9660 primary volume descriptor at sector 16")
    print(f"{src} -> {dst}: {total} sectors")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("img")
    p.add_argument("iso")
    a = p.parse_args()
    convert(a.img, a.iso)


if __name__ == "__main__":
    main()
