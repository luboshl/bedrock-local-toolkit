"""Read-only Always day profile inspection, using only the Python standard library.

Offline: python diagnostics/InspectAlwaysDay.py --snapshot artifacts/nametag-image
Live:    python diagnostics/InspectNametag.py PID --always-day
The live entry point verifies the exact package and PE before reading this profile.
Offline results describe the saved image only, not current game behaviour.
"""
import argparse
import json
import re
import struct
from pathlib import Path


def profile():
    source = (Path(__file__).resolve().parent.parent / "GameMod" / "AlwaysDayProfile.h").read_text()
    function = int(re.search(r"kFunctionRva = (0x[0-9A-Fa-f]+)", source)[1], 16)
    body = re.search(r"kFunctionBytes\[\] = \{(.*?)\};", source, re.S)[1]
    expected = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", body))
    callers = []
    for match in re.finditer(r"\{(0x[0-9A-Fa-f]+), (0x[0-9A-Fa-f]+), (\d+), \{([^}]+)\}\}", source):
        context, return_rva, size = int(match[1], 16), int(match[2], 16), int(match[3])
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", match[4]))
        if len(data) != size or not context < return_rva < context + size:
            raise RuntimeError("Malformed renderer profile")
        callers.append((context, return_rva, data))
    if not callers:
        raise RuntimeError("No renderer callers in profile")
    extra = []
    for name in ("kBrightnessFunction", "kLightImageContext", "kSkyContext", "kStarCalculation", "kCloudContext", "kCloudBinding",
                 "kSkyPhaseFunction", "kSunriseFunction", "kSkyColourFunction", "kSkyBrightnessContext",
                 "kSunriseContext0", "kSunriseContext1", "kSunriseContext2", "kSkyColourContext"):
        rva = int(re.search(rf"{name}Rva = (0x[0-9A-Fa-f]+)", source)[1], 16)
        body = re.search(rf"{name}Bytes\[\] = \{{(.*?)\}};", source, re.S)[1]
        data = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", body))
        extra.append((name, rva, data))
    light_image_return = int(re.search(r"kLightImageReturnRva = (0x[0-9A-Fa-f]+)", source)[1], 16)
    return function, expected, callers, extra, light_image_return


def inspect_read(read, base):
    function, expected, callers, extra, light_image_return = profile()
    actual = read(base + function, len(expected))
    patched = actual[3] == 0xe9 and actual[8:10] == b"\x90\x90"
    original = actual == expected
    remaining = actual[:3] == expected[:3] and actual[10:] == expected[10:]
    source = (Path(__file__).resolve().parent.parent / "GameMod" / "AlwaysDayProfile.h").read_text()
    sites = []
    for name, offset in (("Stars", 1024), ("Cloud", 2048), ("Celestial", 3072)):
        rva = int(re.search(rf"k{name}PatchRva = (0x[0-9A-Fa-f]+)", source)[1], 16)
        body = re.search(rf"k{name}Original\[\] = \{{(.*?)\}};", source, re.S)[1]
        expected_site = bytes(int(value, 16) for value in re.findall(r"0x([0-9A-Fa-f]{2})", body))
        actual_site = read(base + rva, len(expected_site))
        jump = actual_site[0] == 0xe9 and actual_site[5:] == b"\x90" * (len(expected_site) - 5)
        destination = base + rva + 5 + struct.unpack_from("<i", actual_site, 1)[0] if jump else None
        sites.append({"name": name, "rva": rva, "original": expected_site, "offset": offset,
                      "state": "original" if actual_site == expected_site else "jump" if jump else "unexpected",
                      "destination": destination})

    def context_matches(rva, expected_context):
        actual_context = bytearray(read(base + rva, len(expected_context)))
        for site in sites:
            delta = site["rva"] - rva
            first, last = max(0, delta), min(len(actual_context), delta + len(site["original"]))
            if first < last and site["state"] == "jump":
                actual_context[first:last] = site["original"][first-delta:last-delta]
        return actual_context == expected_context

    contexts = [{"contextRva": hex(context), "returnRva": hex(ret),
                 "matches": context_matches(context, data)} for context, ret, data in callers]
    brightness = [{"name": name, "rva": hex(rva), "matches": context_matches(rva, data)}
                  for name, rva, data in extra]
    constants = (struct.unpack("<f", read(base + 0xE7FEF84, 4))[0] == 24000.0 and
                 struct.unpack("<f", read(base + 0xE7E14EC, 4))[0] == -0.25)
    cloud_name = read(base + 0xEBFFFA7, 11) == b"CloudColor\0"
    result = {"functionRva": hex(function), "patchRva": hex(function + 3),
              "functionState": "original" if original else "jump" if patched and remaining else "unexpected",
              "nativeCalculationMatches": remaining, "constantsMatch": constants,
              "cloudUniformNameMatches": cloud_name,
              "rendererContexts": contexts,
              "brightnessContexts": brightness,
              "renderSites": [{"name": s["name"], "rva": hex(s["rva"]), "state": s["state"],
                               "destination": hex(s["destination"]) if s["destination"] is not None else None} for s in sites],
              "unpatchedProfileMatches": original and constants and cloud_name and
              all(item["matches"] for item in contexts + brightness) and
              all(s["state"] == "original" for s in sites)}
    if patched and remaining:
        bridge = base + function + 3 + 5 + struct.unpack_from("<i", actual, 4)[0]
        state = read(bridge + 4096, 112 + 16 * len(callers))
        enabled, version, continuation = struct.unpack_from("<IIQ", state)
        if version != 4:
            result["bridgeCandidate"] = {"address": hex(bridge), "profileVersion": version,
                                         "layoutMatchesProfile": False}
            return result
        addresses = struct.unpack_from(f"<{len(callers)}Q", state, 16)
        counters = struct.unpack_from(f"<{len(callers)}Q", state, 16 + 8 * len(callers))
        ancestor = struct.unpack_from("<Q", state, 16 + 16 * len(callers))[0]
        stars_continuation, cloud_continuation, stars_count, cloud_count = struct.unpack_from("<4Q", state, 24 + 16 * len(callers))
        sky_brightness, sunrise0, sunrise1, sunrise2, sky_colour, celestial_continuation, celestial_count = struct.unpack_from("<7Q", state, 56 + 16 * len(callers))
        sunrise_parents = tuple(int(value, 16) for value in re.findall(r"0x[0-9A-Fa-f]+",
            re.search(r"kSunriseParents\[\] = \{(.*?)\}", source)[1]))
        sky_brightness_return = int(re.search(r"kSkyBrightnessReturnRva = (0x[0-9A-Fa-f]+)", source)[1], 16)
        sky_colour_parent = int(re.search(r"kSkyColourParent = (0x[0-9A-Fa-f]+)", source)[1], 16)
        result["bridgeCandidate"] = {
            "address": hex(bridge), "enabled": enabled,
            "profileVersion": version, "lightImageCaller": hex(ancestor),
            "layoutMatchesProfile": enabled in (0, 1) and ancestor == base + light_image_return and
            sky_brightness == base + sky_brightness_return and sky_colour == base + sky_colour_parent and
            (sunrise0, sunrise1, sunrise2) == tuple(base + ret for ret in sunrise_parents) and
            all(s["state"] == "jump" and s["destination"] == bridge + s["offset"] for s in sites) and
            (stars_continuation, cloud_continuation, celestial_continuation) == tuple(base + s["rva"] + len(s["original"]) for s in sites) and
            continuation == base + function + 10 and addresses == tuple(base + ret for _, ret, _ in callers),
            "starsOverrides": stars_count, "cloudOverrides": cloud_count, "celestialOverrides": celestial_count,
            "noonOverrides": {hex(ret): count for (_, ret, _), count in zip(callers, counters)}}
    return result


def inspect_snapshot(directory):
    root = Path(directory)
    metadata = json.loads((root / "image.json").read_text())
    if metadata.get("timestamp") != "0x6ab54e37" or metadata.get("imageSize") != "0x12c01000":
        raise RuntimeError("Unsupported snapshot identity")
    sections = [(rva, size, (root / (name.lstrip(".") + ".bin")).read_bytes())
                for name, rva, size in metadata["sections"] if name in (".text", ".rdata")]

    def read(address, size):
        for rva, length, data in sections:
            if rva <= address and address + size <= rva + length:
                result = data[address-rva:address-rva+size]
                if len(result) == size:
                    return result
        raise RuntimeError(f"Read outside complete saved sections: {address:#x}")

    result = inspect_read(read, 0)
    result["source"] = "saved image only; no live game verification"
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--snapshot", required=True)
    print(json.dumps(inspect_snapshot(parser.parse_args().snapshot), indent=2))
