#!/usr/bin/env python3
"""Finds the pictures hearted on the handheld. Each folder with favorites has a hearts.txt that lists their names, one a line.
Usage: hearts.py CARD_OR_FOLDER            lists the favorites
       hearts.py CARD_OR_FOLDER --copy DEST  copies them into DEST, one folder of the card per sub folder"""
import os, shutil, sys

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); return 1
    root = args[0]
    dest = args[args.index('--copy') + 1] if '--copy' in args else None
    total = 0
    for folder, _dirs, files in os.walk(root):
        if 'hearts.txt' not in files:
            continue
        with open(os.path.join(folder, 'hearts.txt'), encoding='utf-8', errors='replace') as f:
            names = [l.strip() for l in f if l.strip()]
        rel = os.path.relpath(folder, root)
        for n in names:
            src = os.path.join(folder, n)
            if not os.path.exists(src):
                print(f'missing  {os.path.join(rel, n)}')
                continue
            total += 1
            if dest:
                out = os.path.join(dest, rel if rel != '.' else '')
                os.makedirs(out, exist_ok=True)
                shutil.copy2(src, os.path.join(out, n))
                print(f'copied   {os.path.join(rel, n)}')
            else:
                print(os.path.join(rel, n))
    print(f'{total} favorite picture(s)')
    return 0

if __name__ == '__main__':
    sys.exit(main())
