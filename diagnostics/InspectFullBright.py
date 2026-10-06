"""Read-only Full Bright profile diagnostics, using the pinned local image.

Offline: python diagnostics/InspectFullBright.py --snapshot artifacts/nametag-image
Live:    python diagnostics/InspectNametag.py PID --full-bright
Profile/bridge matches do not verify the visual result in Minecraft.
"""
import argparse
import json
import re
import struct
from pathlib import Path


def profile():
    source = (Path(__file__).resolve().parent.parent / "GameMod" / "FullBrightProfile.h").read_text()
    entries = []
    for name in ("kProducer", "kConsumer"):
        rva = int(re.search(rf"{name}Rva = (0x[0-9A-Fa-f]+)", source)[1], 16)
        body = re.search(rf"{name}Bytes\[\] = \{{(.*?)\}};", source, re.S)[1]
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", body))
        entries.append((rva, data))
    patch = int(re.search(r"kPatchRva = (0x[0-9A-Fa-f]+)", source)[1], 16)
    return entries, patch


def inspect_read(read, base):
    entries, patch = profile()
    producer_rva, producer = entries[0]
    consumer_rva, consumer = entries[1]
    actual = read(base + producer_rva, len(producer))
    offset = patch - producer_rva
    original = actual == producer
    patched = actual[offset] == 0xe9 and actual[offset+5:offset+8] == b"\x90" * 3
    remaining = actual[:offset] == producer[:offset] and actual[offset+8:] == producer[offset+8:]
    consumer_matches = read(base + consumer_rva, len(consumer)) == consumer
    result = {"patchRva": hex(patch), "producerRva": hex(producer_rva),
              "consumerRva": hex(consumer_rva), "consumerMatches": consumer_matches,
              "producerState": "original" if original else "jump" if patched and remaining else "unexpected",
              "unpatchedProfileMatches": original and consumer_matches}
    if patched and remaining:
        bridge = base + patch + 5 + struct.unpack_from("<i", actual, offset+1)[0]
        enabled, version, continuation, overrides = struct.unpack("<IIQQ", read(bridge + 4096, 24))
        result["bridgeCandidate"] = {"address": hex(bridge), "enabled": enabled,
                                     "profileVersion": version, "brightnessOverrides": overrides,
                                     "layoutMatchesProfile": enabled in (0, 1) and version == 1 and
                                     continuation == base + patch + 8}
    return result


def inspect_snapshot(directory):
    root = Path(directory)
    metadata = json.loads((root / "image.json").read_text())
    if metadata.get("timestamp") != "0x6ab54e37" or metadata.get("imageSize") != "0x12c01000":
        raise RuntimeError("Unsupported snapshot identity")
    code = (root / "text.bin").read_bytes()
    rva, size = next((rva, size) for name, rva, size in metadata["sections"] if name == ".text")

    def read(address, count):
        if not rva <= address or address + count > rva + size:
            raise RuntimeError("Read outside saved code")
        value = code[address-rva:address-rva+count]
        if len(value) != count:
            raise RuntimeError("Incomplete saved code")
        return value

    result = inspect_read(read, 0)
    result["source"] = "saved image only; no live game verification"
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", required=True)
    result = inspect_snapshot(parser.parse_args().snapshot)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["unpatchedProfileMatches"] else 1)
