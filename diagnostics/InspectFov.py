"""Read-only FOV/PE/pointer diagnostics. Usage: python diagnostics/InspectFov.py PID.

No process write, injection, debugger, or dependency outside Python's standard library.
The result can be saved and compared across independent Minecraft launches.
"""
import ctypes as c
from ctypes import wintypes as w
import json
import math
import re
import struct
import sys
import time

EXPECTED_PACKAGE = "Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe"


class MemoryInfo(c.Structure):
    _fields_ = [("base", c.c_void_p), ("allocation", c.c_void_p),
                ("allocation_protect", w.DWORD), ("partition", w.WORD),
                ("size", c.c_size_t), ("state", w.DWORD),
                ("protect", w.DWORD), ("type", w.DWORD)]


def inspect(pid):
    k = c.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k.OpenProcess.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t,
                                   c.POINTER(c.c_size_t)]
    k.VirtualQueryEx.argtypes = [w.HANDLE, c.c_void_p, c.POINTER(MemoryInfo), c.c_size_t]
    k.VirtualQueryEx.restype = c.c_size_t
    k.K32EnumProcessModules.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
    k.GetPackageFullName.argtypes = [w.HANDLE, c.POINTER(w.UINT), w.LPWSTR]
    handle = k.OpenProcess(0x0410, False, pid)
    if not handle:
        raise c.WinError(c.get_last_error())
    try:
        length = w.UINT(0)
        if k.GetPackageFullName(handle, c.byref(length), None) != 122:
            raise RuntimeError("Cannot verify package")
        package = c.create_unicode_buffer(length.value)
        if k.GetPackageFullName(handle, c.byref(length), package) or package.value != EXPECTED_PACKAGE:
            raise RuntimeError("Unsupported package; no memory scanned")

        def read(address, size):
            buf = c.create_string_buffer(size)
            count = c.c_size_t()
            if not k.ReadProcessMemory(handle, address, buf, size, c.byref(count)) or count.value != size:
                raise RuntimeError(f"Incomplete read at {address:#x}: {c.get_last_error()}")
            return buf.raw

        def query(address):
            info = MemoryInfo()
            if not k.VirtualQueryEx(handle, address, c.byref(info), c.sizeof(info)):
                raise c.WinError(c.get_last_error())
            return info

        modules = (c.c_void_p * 1024)()
        needed = w.DWORD()
        if not k.K32EnumProcessModules(handle, modules, c.sizeof(modules), c.byref(needed)):
            raise c.WinError(c.get_last_error())
        base = modules[0]
        header = read(base, 4096)
        pe = struct.unpack_from("<I", header, 0x3c)[0]
        count = struct.unpack_from("<H", header, pe + 6)[0]
        timestamp = struct.unpack_from("<I", header, pe + 8)[0]
        image_size = struct.unpack_from("<I", header, pe + 24 + 56)[0]
        section_table = pe + 24 + struct.unpack_from("<H", header, pe + 20)[0]
        sections = []
        for i in range(count):
            offset = section_table + i * 40
            size, rva = struct.unpack_from("<II", header, offset + 8)
            sections.append((header[offset:offset+8].rstrip(b"\0").decode(), rva, size))

        def describe(address):
            info = query(address)
            return {"address": hex(address), "allocation": hex(info.allocation or 0),
                    "region": hex(info.base or 0), "size": info.size,
                    "type": hex(info.type), "protect": hex(info.protect)}

        chains = []
        roots = [(0x11D21990, [0, 0x158, 0x260, 0x188]),
                 (0x11D1FD08, [0, 0x180, 0x740, 0x188]),
                 (0x11D33088, [0, 0x2A8, 0x2D8, 0x6C8]),
                 (0x11D33088, [0, 0x2A8, 0x28F0, 0x188])]
        for root, offsets in roots:
            chain = {"rootRva": hex(root), "offsets": list(map(hex, offsets)), "steps": []}
            cursor = base + root
            try:
                for offset in offsets:
                    slot = cursor + offset
                    cursor = struct.unpack("<Q", read(slot, 8))[0]
                    chain["steps"].append({"slot": hex(slot), "value": hex(cursor),
                                           "memory": describe(cursor) if cursor else None})
                    if not cursor or cursor & 7:
                        raise RuntimeError("Not an aligned non-null pointer")
                chain["target"] = hex(cursor + 16)
                chain["floats"] = struct.unpack("<5f", read(cursor + 16, 20))
            except (OSError, RuntimeError) as error:
                chain["error"] = str(error)
            chains.append(chain)

        root_refs = {hex(root): [] for root, _ in roots}
        type_names = []
        for name, rva, size in sections:
            if name not in (".text", ".data", ".rdata"):
                continue
            data = read(base + rva, size)
            if name == ".text":
                for match in re.finditer(rb"[\x48-\x4f][\x8b\x8d\x89][\x05\x0d\x15\x1d\x25\x2d\x35\x3d]", data):
                    offset = match.start()
                    target = rva + offset + 7 + struct.unpack_from("<i", data, offset + 3)[0]
                    if hex(target) in root_refs:
                        root_refs[hex(target)].append({"instructionRva": hex(rva + offset),
                                                       "bytes": data[max(0, offset-16):offset+32].hex()})
            for match in re.finditer(rb"\.\?AV[^\x00]{0,160}\x00", data):
                text = match.group()[:-1].decode("ascii", errors="replace")
                if re.search("FloatOption|Fov|FOV|Options@@|Option@@|OptionManager|ClientInstance@@|Allocator", text):
                    type_names.append({"rva": hex(rva+match.start()), "name": text})

        candidates = []
        options_candidates = []
        cursor = 0x10000
        bytes_read = 0
        started = time.monotonic()
        prefix = struct.pack("<2f", 30, 110)
        while cursor < 0x7fffffff0000:
            if time.monotonic() - started > 60:
                raise RuntimeError("Scan timed out; result is unverified")
            info = query(cursor)
            end = (info.base or 0) + info.size
            if end <= cursor:
                raise RuntimeError("Invalid memory map")
            if info.state == 0x1000 and info.type == 0x20000 and info.protect == 4:
                while cursor < end:
                    size = min(4 * 1024 * 1024, end-cursor)
                    data = read(cursor, min(size+16, end-cursor))
                    bytes_read += size
                    option_signature = struct.pack("<Q", base + 0xE7FC490)
                    option_offset = data.find(option_signature)
                    while 0 <= option_offset < size:
                        option_address = cursor + option_offset
                        if option_address % 8 == 0:
                            try:
                                wrapper = struct.unpack("<Q", read(option_address+0x188, 8))[0]
                                value_vtable, owner = struct.unpack("<2Q", read(wrapper, 16))
                                key_pointer, _, key_size, capacity = struct.unpack("<4Q", read(owner+0x188, 32))
                                key = read(key_pointer, 18) if key_size == 17 and 17 <= capacity <= 4096 else b""
                                values = struct.unpack("<5f", read(wrapper+16, 20))
                                display = read(owner+0x1A8, 32)
                                caption = read(owner+0x1D8, 32)
                                if (value_vtable == base+0xE778EF0 and key == b"gfx_field_of_view\0" and
                                        display[:12] == b"fieldOfView\0" and caption[:12] == b"options.fov\0"):
                                    options_candidates.append({"options": hex(option_address), "pattern": hex(wrapper+16),
                                                               "owner": hex(owner), "key": key[:-1].decode(), "values": values})
                            except (OSError, RuntimeError):
                                pass
                        option_offset = data.find(option_signature, option_offset+8)
                    offset = data.find(prefix)
                    while 0 <= offset < size and offset + 20 <= len(data):
                        values = struct.unpack_from("<5f", data, offset)
                        if (cursor+offset) % 4 == 0 and all(map(math.isfinite, values)) and 10 <= values[2] <= 120 and abs(values[3]-60) < .0001 and abs(values[4]-.001) < .000001:
                            address = cursor+offset
                            nearby = read(address-32, 128)
                            pointers = struct.unpack("<16Q", nearby)
                            candidates.append({"pattern": hex(address), "values": values,
                                               "memory": describe(address), "nearbyHex": nearby.hex(),
                                               "imagePointers": [{"offset": index*8-32, "rva": hex(value-base)}
                                                                 for index, value in enumerate(pointers)
                                                                 if base <= value < base+image_size]})
                        offset = data.find(prefix, offset+4)
                    cursor += size
            cursor = end
        return {"pid": pid, "package": package.value, "moduleBase": hex(base),
                "imageSize": hex(image_size), "timestamp": hex(timestamp),
                "sections": sections, "chains": chains, "rootReferences": root_refs,
                "types": type_names, "candidates": candidates, "optionsCandidates": options_candidates,
                "bytesRead": bytes_read,
                "elapsedSeconds": round(time.monotonic()-started, 3)}
    finally:
        k.CloseHandle(handle)


if __name__ == "__main__":
    print(json.dumps(inspect(int(sys.argv[1])), indent=2, allow_nan=False))
