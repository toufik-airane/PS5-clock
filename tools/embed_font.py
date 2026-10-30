#!/usr/bin/env python3
"""Embed the bundled OFL font so the executable needs no runtime assets."""
import argparse
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('source', type=Path, help='Bundled assets/Outfit-Light.ttf')
    parser.add_argument('destination', type=Path, help='Generated C header')
    args = parser.parse_args()
    data = args.source.read_bytes()
    args.destination.parent.mkdir(parents=True, exist_ok=True)
    with args.destination.open('w', encoding='ascii', newline='\n') as out:
        out.write('/* Generated from assets/Outfit-Light.ttf; see assets/OFL.txt. */\n')
        out.write('static const unsigned char clock_font_data[] = {\n')
        for offset in range(0, len(data), 16):
            out.write('    ' + ','.join(str(b) for b in data[offset:offset + 16]) + ',\n')
        out.write('};\n')


if __name__ == '__main__':
    main()
