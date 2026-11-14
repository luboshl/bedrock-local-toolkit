"""Exercise live-profile decoding with synthetic memory; never open a game."""
import importlib.util
import struct
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("alwaysday", Path(__file__).resolve().parents[1] / "diagnostics/InspectAlwaysDay.py")
inspector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(inspector)


class InspectionTests(unittest.TestCase):
    def setUp(self):
        self.memory = {}
        self.function, expected, self.callers, extra, self.light_parent = inspector.profile()
        self.write(self.function, expected)
        for context, _, data in self.callers:
            self.write(context, data)
        for _, rva, data in extra:
            self.write(rva, data)
        self.write(0xf1527ac, struct.pack("<f", 24000))
        self.write(0xf1527b0, struct.pack("<f", -0.25))
        self.write(0xf563cb0, b"CloudColor\0")

    def write(self, address, data):
        self.memory.update((address + i, value) for i, value in enumerate(data))

    def read(self, address, size):
        return bytes(self.memory[address + i] for i in range(size))

    def test_original_and_changed_context(self):
        self.assertTrue(inspector.inspect_read(self.read, 0)["unpatchedProfileMatches"])
        self.write(0x212e1f0, b"\xCC")
        self.assertFalse(inspector.inspect_read(self.read, 0)["unpatchedProfileMatches"])

    def test_installed_layout_and_partial_context_overlap(self):
        bridge = 0x10000000
        sites = [(self.function + 3, 7, 0), (0x539331b, 8, 1536),
                 (0x67376a3, 7, 2048), (0x53933c2, 8, 3072)]
        for address, size, offset in sites:
            self.write(address, b"\xE9" + struct.pack("<i", bridge + offset - address - 5) + b"\x90" * (size - 5))
        n = len(self.callers)
        state = bytearray(160 + 16 * n)
        struct.pack_into("<IIQ", state, 0, 1, 5, self.function + 10)
        struct.pack_into(f"<{n}Q", state, 16, *(ret for _, ret, _ in self.callers))
        struct.pack_into(f"<{n}Q", state, 16 + 8 * n, *range(n))
        struct.pack_into("<5Q", state, 16 + 16 * n, self.light_parent, 0x5393323, 0x67376aa, 7, 8)
        struct.pack_into("<7Q", state, 56 + 16 * n, 0x5393418, 0x53939f2, 0x5394677,
                         0x5394908, 0x53946b2, 0x53933ca, 9)
        struct.pack_into("<6Q", state, 112 + 16 * n, 0x5376b66, 0x5aed112,
                         0x53742d4, 0x53759c2, 0x5374252, 0x5374271)
        self.write(bridge + 4096, state)
        result = inspector.inspect_read(self.read, 0)
        self.assertTrue(all(item["matches"] for item in result["rendererContexts"] + result["brightnessContexts"]))
        self.assertTrue(result["bridgeCandidate"]["layoutMatchesProfile"])
        self.assertEqual(result["bridgeCandidate"]["celestialOverrides"], 9)
        for offset in range(112, 160, 8):
            address = bridge + 4096 + offset + 16 * n
            original = self.read(address, 8)
            self.write(address, struct.pack("<Q", 0x1234))
            self.assertFalse(inspector.inspect_read(self.read, 0)["bridgeCandidate"]["layoutMatchesProfile"])
            self.write(address, original)
        self.write(bridge + 4096 + 56 + 16 * n, struct.pack("<Q", 0x1234))
        self.assertFalse(inspector.inspect_read(self.read, 0)["bridgeCandidate"]["layoutMatchesProfile"])
        self.write(bridge + 4100, struct.pack("<I", 3))
        self.assertFalse(inspector.inspect_read(self.read, 0)["bridgeCandidate"]["layoutMatchesProfile"])


if __name__ == "__main__":
    unittest.main()
