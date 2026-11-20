"""Read-only checks of the ported 1.26.6501.0 profiles against an installed PE.

Usage: python diagnostics/InspectBuildProfile.py "C:/XboxGames/Minecraft for Windows/Content/Minecraft.Windows.exe"
Uses only the Python standard library. Never opens a process or modifies files.
The result covers all current native profiles and their static bindings.
"""
import argparse
import json
from pathlib import Path
import re
import struct


def inspect(path):
    data = Path(path).read_bytes()
    if data[:2] != b"MZ":
        raise ValueError("Invalid DOS signature")
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    if data[pe:pe + 4] != b"PE\0\0":
        raise ValueError("Invalid PE signature")
    machine, count, timestamp = struct.unpack_from("<HHI", data, pe + 4)
    optional = pe + 24
    image_size = struct.unpack_from("<I", data, optional + 56)[0]
    if (machine, timestamp, image_size) != (0x8664, 0x6AFA31EC, 0x1383E000):
        raise ValueError("Unsupported PE identity; no profiles checked")
    if struct.unpack_from("<H", data, optional)[0] != 0x20B:
        raise ValueError("Expected PE32+ image")
    image_base = struct.unpack_from("<Q", data, optional + 24)[0]
    table = optional + struct.unpack_from("<H", data, pe + 20)[0]
    sections = [struct.unpack_from("<8sIIIIIIHHI", data, table + i * 40)
                for i in range(count)]

    def read(rva, size):
        for _, virtual_size, start, raw_size, raw, *_ in sections:
            if start <= rva and rva + size <= start + min(virtual_size, raw_size):
                result = data[raw + rva - start:raw + rva - start + size]
                if len(result) == size:
                    return result
        raise ValueError(f"Incomplete PE read at {rva:#x}")

    root = Path(__file__).resolve().parents[1]
    checks = []

    def check(name, rva, expected):
        checks.append({"name": name, "rva": hex(rva), "size": len(expected),
                       "matches": read(rva, len(expected)) == expected})

    def source(name):
        text = (root / "GameMod" / name).read_text()
        constants = {key: int(value, 16) for key, value in re.findall(
            r"constexpr uintptr_t (k\w+) = (0x[0-9A-Fa-f]+);", text)}
        arrays = {key: bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", body))
                  for key, body in re.findall(
                      r"constexpr (?:unsigned char|std::array<unsigned char, \d+>) (k\w+)(?:\[\])? = \{(.*?)\};",
                      text, re.S)}
        return constants, arrays

    constants, arrays = source("ScreenInputProfile.h")
    for key, expected in arrays.items():
        check(key, constants[key.removesuffix("Bytes") + "Rva"], expected)
    for owner, slot, method in (("Client", 0x730, "ClientStack"), ("Game", 0x3C8, "GameStack"),
                                ("Stack", 0x1A8, "TopScene"), ("Scene", 0x1D0, "SceneName"),
                                ("Scene", 0x288, "Transition"), ("Background", 0x1B8, "BackgroundName"),
                                ("Background", 0x118, "False"), ("Background", 0x230, "False"),
                                ("Background", 0x288, "False")):
        check(f"{owner} slot {slot:#x}", constants[f"k{owner}VtableRva"] + slot,
              struct.pack("<Q", image_base + constants[f"k{method}Rva"]))
    check("Background name", constants["kBackgroundNameStringRva"], b"cubemap_background_screen\0")
    constants, arrays = source("FpsProfile.h")
    check("FPS update entry", constants["kMinecraftGameUpdateRva"], arrays["kMinecraftGameUpdateEntry"])
    caller = constants["kUpdateCallerRva"]
    check("FPS caller", caller, arrays["kUpdateCaller"])
    destination = caller + 8 + struct.unpack("<i", read(caller + 4, 4))[0]
    checks.append({"name": "FPS call destination", "matches": destination == constants["kMinecraftGameUpdateRva"]})
    constants, arrays = source("FullBrightProfile.h")
    for label in ("Producer", "Consumer"):
        check("Full Bright " + label, constants[f"k{label}Rva"], arrays[f"k{label}Bytes"])
    check("Full Bright patch", constants["kPatchRva"], arrays["kOriginal"])
    check("Mouse wheel consumer", 0x331790, bytes.fromhex("55 56 57 48 83 ec 60 48 8d 6c 24 60 0f 29 75 f0"))
    check("Mouse event consumer", 0x427530, bytes.fromhex("41 57 41 56 41 55 41 54 56 57 55 53 48 83 ec 48"))
    check("Wheel delta", 0x8C6D0, bytes.fromhex("8b 9d 10 01 00 00 41 2b 5f 40 66 85 db"))
    for site, target in ((0x8C746, 0x331790), (0x8C77F, 0x427530)):
        check("Mouse call", site, b"\xe8" + struct.pack("<i", target - site - 5))
    from InspectAlwaysDay import profile
    function, expected, callers, extra, _ = profile()
    check("Always day function", function, expected)
    check("Always day cycle constant", 0xF1537AC, struct.pack("<f", 24000.0))
    check("Always day phase constant", 0xF1537B0, struct.pack("<f", -0.25))
    check("Always day cloud uniform name", 0xF564D36, b"CloudColor\0")
    for context, ret, expected in callers:
        check("Always day caller", context, expected)
        checks.append({"name": "Always day return boundary", "rva": hex(ret),
                       "matches": read(ret - 6, 2) == b"\xff\x15" or read(ret - 5, 1) == b"\xe8"})
    for name, rva, expected in extra:
        check("Always day " + name, rva, expected)
    constants, arrays = source("NametagHudProfile.h")
    pairs = {"kGateOriginal": "kGateRva", "kLoopExitOriginal": "kLoopExitRva",
             "kProducerFrame": "kProducerFrameRva", "kLocalPlayer": "kLocalPlayerRva",
             "kGateContext": "kGateContextRva", "kLoopExitContext": "kLoopExitContextRva",
             "kOtherLabels": "kOtherLabelsContextRva", "kCleanupContext": "kCleanupContextRva",
             "kGetterContext": "kGetterContextRva", "kNamesMaskOriginal": "kNamesMaskRva",
             "kMetadataFrame": "kMetadataFrameRva", "kNamesSource": "kNamesSourceRva",
             "kNamesMaskContext": "kNamesMaskContextRva", "kNamesGetter": "kNamesGetterRva",
             "kSplitNamesGetter": "kSplitNamesGetterRva"}
    for name, key in pairs.items():
        check("Nametag " + name, constants[key], arrays[name])
    for name in ("Getter", "NamesGetter", "SplitNamesGetter"):
        check("Nametag " + name + " binding", constants[f"k{name}SlotRva"],
              struct.pack("<Q", image_base + constants[f"k{name}Rva"]))
    check("Nametag own gate", 0x5397F2C, bytes.fromhex("48 3b 9d b8 00 00 00 74 db"))
    check("Nametag depth argument", 0x5950F9E, bytes.fromhex("c6 44 24 50 00"))
    check("Nametag material selector", 0x57E41A9,
          bytes.fromhex("80 bd 60 01 00 00 00 48 0f 45 c1 49 0f 45 d0 48 01 d8 48 01 da"))
    return {"build": "1.26.6501.0", "checks": checks,
            "portedProfilesMatch": all(item["matches"] for item in checks),
            "unportedFeatures": [],
            "scope": "Static instructions and bindings; does not establish in-game behavior"}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("image")
    result = inspect(parser.parse_args().image)
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["portedProfilesMatch"] else 1)
