#!/usr/bin/env python3
"""Stage the native client for the Memory Stick: dist/PSP/GAME/KOMI_NATIVE.

Program files only (EBOOT.PBP with MEMSIZE=1, roots.pem, fonts/). The
files the app writes beside itself -- komi-mylist.txt, komi-wifi.txt --
are never part of the package, so installing over an old copy keeps them.
"""
from pathlib import Path
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parent))
from package import ROOT, set_pbp_title  # noqa: E402

SOURCE = ROOT / 'vendor/tilefinch/build-preset-psp/komi-app'
DEST = ROOT / 'dist/PSP/GAME/KOMI_NATIVE'
TITLE = 'komi-tube（新）'
FILES = ['EBOOT.PBP', 'roots.pem', 'fonts/TilefinchSans-Regular.ttf',
         'fonts/LICENSE-TilefinchSans.txt', 'fonts/LICENSE-Unifont.txt']


def main():
    if DEST.exists():
        shutil.rmtree(DEST)
    for name in FILES:
        target = DEST / name
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(SOURCE / name, target)
    set_pbp_title(DEST / 'EBOOT.PBP', TITLE,
                  (ROOT / 'assets/ICON0.PNG').read_bytes())
    for name in FILES:
        print(f'{DEST.relative_to(ROOT)}/{name}')


if __name__ == '__main__':
    main()
