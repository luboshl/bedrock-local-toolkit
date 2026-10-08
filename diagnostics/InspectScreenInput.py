"""Check the read-only screen input profile against a supported image snapshot.

This validates native instructions and vtable bindings, not live chat behavior.
"""
import argparse
import json
import re
import struct
from pathlib import Path


def inspect_snapshot(directory):
    root = Path(directory)
    metadata = json.loads((root / "image.json").read_text())
    if metadata.get("timestamp") != "0x6ab54e37" or metadata.get("imageSize") != "0x12c01000":
        raise RuntimeError("Unsupported snapshot identity")
    base = int(metadata["moduleBase"], 16)
    sections = [(base + rva, size, (root / f"{name[1:]}.bin").read_bytes())
                for name, rva, size in metadata["sections"] if name in (".text", ".rdata")]

    def read(address, size):
        for start, length, data in sections:
            if start <= address and address + size <= start + length:
                result = data[address - start:address - start + size]
                if len(result) == size:
                    return result
        raise RuntimeError("Incomplete snapshot read")

    source = (Path(__file__).resolve().parents[1] / "GameMod/ScreenInputProfile.h").read_text()
    constants = {name: int(value, 16) for name, value in
                 re.findall(r"constexpr uintptr_t (k\w+) = (0x[0-9A-Fa-f]+);", source)}
    checks = []
    for name, body in re.findall(r"constexpr unsigned char (k\w+)Bytes\[\] = \{(.*?)\};", source, re.S):
        expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", body))
        rva = constants[name + "Rva"]
        checks.append({"context": name, "rva": hex(rva), "size": len(expected),
                       "matches": read(base + rva, len(expected)) == expected})
    for table, slot, method in (("Client", 0x748, "ClientStack"), ("Game", 0x3C8, "GameStack"),
                                ("Stack", 0x1A8, "TopScene"), ("Scene", 0x1D0, "SceneName"),
                                ("Scene", 0x280, "Passthrough")):
        address = base + constants[f"k{table}VtableRva"] + slot
        checks.append({"context": f"{table} vtable + {slot:#x}",
                       "matches": struct.unpack("<Q", read(address, 8))[0] == base + constants[f"k{method}Rva"]})
    return {"source": "supported saved image; no live game verification", "checks": checks,
            "profileMatches": all(check["matches"] for check in checks)}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", required=True)
    result = inspect_snapshot(parser.parse_args().snapshot)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["profileMatches"] else 1)
