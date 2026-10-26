"""Independently authored protocol examples; no vendor executable or capture needed."""

from pathlib import Path
import struct
import sys


def text(value, size):
    encoded = value.encode("ascii")
    assert len(encoded) < size
    return encoded.ljust(size, b"\0")


def packet(opcode, payload):
    return struct.pack("<4sBBHI", b"BATC", opcode, 4, 12 + len(payload), 42) + payload


def generate(directory):
    directory.mkdir(parents=True, exist_ok=True)
    ident = struct.pack("<I", 7)
    coordinates = struct.pack("<dd", 35.877639, -78.787472)
    dataref = text("sim/cockpit2/radios/actuators/com1_frequency_hz_833", 256)
    examples = {
        "BuildSpawn": (1, ident + text("BB_A320", 8) + text("BB_", 4) + text("TEST", 16)
                       + coordinates + struct.pack("<ffB", 435.25, 237.5, 1)
                       + text("BB_A320_AFR", 64)),
        "BuildUpdate": (2, ident + coordinates + struct.pack("<ffffffBB", 435.25, 237.5, .5, .5, 25, -2, 3, 7)),
        "BuildRemove": (3, ident),
        "BuildLights": (4, ident + bytes((1, 0, 1, 1, 0))),
        "BuildGear": (5, ident + struct.pack("<BfBB", 1, .5, 0, 0)),
        "BuildCallsign": (6, ident + text("TEST123", 16)),
        "BuildSsrMode": (8, ident + b"\x04"),
        "BuildRemoveAll": (9, b""),
        "BuildSetDataref": (10, struct.pack("<Bf", 0, .5) + dataref),
        "BuildSetDatarefFloat": (10, struct.pack("<Bf", 0, .5) + dataref),
        "BuildSetDatarefInt": (10, struct.pack("<Bi", 1, 121000) + dataref),
        "BuildSetPlayerDatarefs": (11, b"\0" * (4 * 256)),
        "BuildObjectUpdate": (12, ident + coordinates + struct.pack("<fB", 90, 0)
                              + text("lib/airport/vehicles/test.obj", 64)),
        "BuildObjectRemove": (13, ident),
        "BuildHeartbeat": (127, struct.pack("<BBI", 4, 4, 12345)),
    }
    for name, (opcode, payload) in examples.items():
        (directory / (name + ".bin")).write_bytes(packet(opcode, payload))


if __name__ == "__main__":
    generate(Path(sys.argv[1]))
