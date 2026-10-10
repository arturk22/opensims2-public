#!/usr/bin/env python3
"""Rebuild the installed The Sims 2 tree from the EA installer zips on the discs.

Each disc carries a compressed.zip (Windows-style backslash paths); disc 4 also
carries per-language <lang>_compressed.zip overlays that are applied last.
"""
import argparse
import os
import shutil
import sys
import zipfile


def safe_dest(root, name):
    parts = [p for p in name.replace("\\", "/").split("/") if p not in ("", ".")]
    if not parts or any(p == ".." for p in parts):
        sys.exit(f"refusing suspicious path: {name!r}")
    return os.path.join(root, *parts)


def extract(zip_path, out):
    count = 0
    with zipfile.ZipFile(zip_path) as z:
        for info in z.infolist():
            if info.is_dir() or info.filename.endswith("\\"):
                continue
            dest = safe_dest(out, info.filename)
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            with z.open(info) as src, open(dest, "wb") as dst:
                shutil.copyfileobj(src, dst, 1 << 20)
            if os.path.getsize(dest) != info.file_size:
                sys.exit(f"{dest}: size mismatch")
            count += 1
    print(f"{zip_path}: {count} files")


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("data_dir", help="directory containing disc1..disc4")
    p.add_argument("out", help="output directory for the installed tree")
    p.add_argument("--lang", default="pt-br")
    a = p.parse_args()

    discs = sorted(d for d in os.listdir(a.data_dir) if d.startswith("disc"))
    for d in discs:
        z = os.path.join(a.data_dir, d, "compressed.zip")
        if os.path.exists(z):
            extract(z, a.out)
    for d in discs:
        z = os.path.join(a.data_dir, d, f"{a.lang}_compressed.zip")
        if os.path.exists(z):
            extract(z, a.out)


if __name__ == "__main__":
    main()
