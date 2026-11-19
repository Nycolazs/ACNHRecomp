#!/usr/bin/env python3
"""Build a LayeredFS mod for a new island with two specific starting villagers.

Example for Jitters and Phoebe, using a test installation and a fresh save:

  python3 tools/play/starter_pair.py /path/to/romfs brd04 ost10 /path/to/test/mods/starter-pair
  NX_MODS=starter-pair /path/to/acnh /path/to/test

The input is the player's own extracted RomFS. The generated mod contains game data
and belongs in the local test folder.
"""

import argparse
import struct
from pathlib import Path


TABLE = Path("Bcsv/NmlNpcParam.bcsv")
JOCK = 0x6641BFDA
SISTERLY = 0x70706744


def select_pair(data, jock, sisterly):
    if len(data) < 28 or data[12:16] != b"VSCB":
        raise ValueError("expected an NmlNpcParam BCSV header")
    rows, stride, columns = struct.unpack_from("<IIH", data)
    start = 28 + columns * 8
    if not stride or start + rows * stride != len(data):
        raise ValueError("invalid BCSV record layout")
    offsets = dict(struct.iter_unpack("<II", data[28:start]))
    # Label, NpcLooks and InitLive are identified by their BCSV field hashes.
    try:
        label, personality, eligible = (offsets[key] for key in (0x2F1B930D, 0x04F52F6A, 0x454B2ADC))
    except KeyError as exc:
        raise ValueError("missing a starter selection field") from exc
    if label + 6 > stride or personality + 4 > stride or eligible >= stride:
        raise ValueError("starter selection field outside its record")

    wanted = {jock: JOCK, sisterly: SISTERLY}
    if len(wanted) != 2:
        raise ValueError("choose two different villagers")
    found = set()
    for row in range(start, len(data), stride):
        name = data[row + label:row + label + 6].split(b"\0", 1)[0].decode("ascii")
        if name in wanted:
            looks, = struct.unpack_from("<I", data, row + personality)
            if name in found or not data[row + eligible] or looks != wanted[name]:
                raise ValueError(f"{name} is ineligible for the selected starter slot")
            found.add(name)
        else:
            data[row + eligible] = 0
    if wanted.keys() - found:
        raise ValueError("unknown villager(s): " + ", ".join(sorted(wanted.keys() - found)))


def main():
    args = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    args.add_argument("romfs", type=Path)
    args.add_argument("jock")
    args.add_argument("sisterly")
    args.add_argument("mod", type=Path, help="new test mod directory")
    opts = args.parse_args()
    source, output = opts.romfs / TABLE, opts.mod / "romfs" / TABLE
    try:
        data = bytearray(source.read_bytes())
        select_pair(data, opts.jock, opts.sisterly)
        output.parent.mkdir(parents=True, exist_ok=True)
        with output.open("xb") as f:
            f.write(data)
    except (OSError, ValueError) as exc:
        args.exit(1, f"{exc}\n")
    print(output)


if __name__ == "__main__":
    main()
