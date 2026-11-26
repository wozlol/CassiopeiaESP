#!/usr/bin/env python3
# Usage: a11convert.py image|music INPUT [OUTFOLDER]
# Makes files the media viewer plays. Needs ffmpeg.
#   image  -> baseline JPEG that fits 480x320, quality about 85
#   music  -> MP3, 44.1 kHz stereo 128 kbit (the player mixes it down to the single speaker)
# Output goes to OUTFOLDER, default ./a11-out. Copy it to /media on the SD card.
import sys, os, subprocess

def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ('image', 'music'):
        sys.exit(__doc__)
    kind, src = sys.argv[1], sys.argv[2]
    out = sys.argv[3] if len(sys.argv) > 3 else 'a11-out'
    os.makedirs(out, exist_ok=True)
    stem = os.path.splitext(os.path.basename(src))[0].replace(' ', '_')
    if kind == 'image':
        dst = os.path.join(out, stem + '.jpg')
        cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-vf', 'scale=480:320:force_original_aspect_ratio=decrease', '-frames:v', '1', '-q:v', '3', '-pix_fmt', 'yuvj420p', dst]
    else:
        dst = os.path.join(out, stem + '.mp3')
        cmd = ['ffmpeg', '-y', '-loglevel', 'error', '-i', src, '-vn', '-ar', '44100', '-ac', '2', '-b:a', '128k', dst]
    subprocess.run(cmd, check=True)
    print(dst)

if __name__ == '__main__':
    main()
