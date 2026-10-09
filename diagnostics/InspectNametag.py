"""Read-only Nametag profile diagnostics for the pinned Minecraft process.

Usage: python diagnostics/InspectNametag.py PID
       python diagnostics/InspectNametag.py PID --options 0xLIVE_OPTIONS_ADDRESS
       python diagnostics/InspectNametag.py PID --rva 0x123 --size 4096 --coff artifacts/code.obj

Uses only the Python standard library and PROCESS_QUERY_INFORMATION | VM_READ.
The optional COFF file is a local snapshot for Microsoft's dumpbin /disasm.
No game writes, DLL loading, third-party disassembler, or network access.
"""
import argparse
import ctypes as c
from ctypes import wintypes as w
import json
import struct
from pathlib import Path

PACKAGE = "Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe"
IMAGE_SIZE = 0x12C01000
TIMESTAMP = 0x6AB54E37


def inspect(args):
    k = c.WinDLL("kernel32", use_last_error=True)
    k.OpenProcess.argtypes = [w.DWORD, w.BOOL, w.DWORD]
    k.OpenProcess.restype = w.HANDLE
    k.CloseHandle.argtypes = [w.HANDLE]
    k.ReadProcessMemory.argtypes = [w.HANDLE, c.c_void_p, c.c_void_p, c.c_size_t,
                                   c.POINTER(c.c_size_t)]
    k.K32EnumProcessModules.argtypes = [w.HANDLE, c.c_void_p, w.DWORD, c.POINTER(w.DWORD)]
    k.GetPackageFullName.argtypes = [w.HANDLE, c.POINTER(w.UINT), w.LPWSTR]
    handle = k.OpenProcess(0x0410, False, args.pid)
    if not handle:
        raise c.WinError(c.get_last_error())
    try:
        length = w.UINT()
        if k.GetPackageFullName(handle, c.byref(length), None) != 122:
            raise RuntimeError("Cannot verify package")
        package = c.create_unicode_buffer(length.value)
        if k.GetPackageFullName(handle, c.byref(length), package) or package.value != PACKAGE:
            raise RuntimeError("Unsupported package; no code scanned")

        def read(address, size):
            buf = c.create_string_buffer(size)
            count = c.c_size_t()
            if not k.ReadProcessMemory(handle, address, buf, size, c.byref(count)) or count.value != size:
                raise RuntimeError(f"Incomplete read at {address:#x}: {c.get_last_error()}")
            return buf.raw

        modules = (c.c_void_p * 1024)()
        needed = w.DWORD()
        if not k.K32EnumProcessModules(handle, modules, c.sizeof(modules), c.byref(needed)):
            raise c.WinError(c.get_last_error())
        base = modules[0]
        header = read(base, 4096)
        pe = struct.unpack_from("<I", header, 0x3c)[0]
        if header[:2] != b"MZ" or header[pe:pe+4] != b"PE\0\0":
            raise RuntimeError("Invalid PE header")
        machine, count, timestamp = struct.unpack_from("<HHI", header, pe+4)
        image_size = struct.unpack_from("<I", header, pe+24+56)[0]
        if machine != 0x8664 or timestamp != TIMESTAMP or image_size != IMAGE_SIZE:
            raise RuntimeError("Unsupported PE identity; no code scanned")
        table = pe+24+struct.unpack_from("<H", header, pe+20)[0]
        sections = []
        for i in range(count):
            offset = table+i*40
            size, rva = struct.unpack_from("<II", header, offset+8)
            sections.append((header[offset:offset+8].rstrip(b"\0").decode(), rva, size))

        if args.snapshot:
            directory = Path(args.snapshot)
            directory.mkdir(parents=True, exist_ok=True)
            for name, rva, size in sections:
                if name in (".text", ".rdata", ".pdata", ".data"):
                    (directory/(name[1:]+".bin")).write_bytes(read(base+rva, size))
            metadata = {"pid": args.pid, "moduleBase": hex(base), "timestamp": hex(timestamp),
                        "imageSize": hex(image_size), "sections": sections}
            (directory/"image.json").write_text(json.dumps(metadata, indent=2))
            return metadata

        if args.coff:
            if args.rva is None or not 0 < args.size <= 1024*1024:
                raise RuntimeError("COFF snapshot requires an RVA and a size <= 1 MiB")
            if not any(rva <= args.rva and args.rva+args.size <= rva+size
                       for name, rva, size in sections if name == ".text"):
                raise RuntimeError("Snapshot must stay inside .text")
            data = read(base+args.rva, args.size)
            coff = struct.pack("<HHIIIHH", 0x8664, 1, 0, 0, 0, 0, 0)
            section = struct.pack("<8sIIIIIIHHI", b".text\0\0\0", 0, args.rva,
                                  len(data), 60, 0, 0, 0, 0, 0x60500020)
            Path(args.coff).write_bytes(coff+section+data)
            return {"pid": args.pid, "moduleBase": hex(base), "rva": hex(args.rva),
                    "size": args.size, "coff": str(Path(args.coff).resolve())}

        if args.full_bright:
            from InspectFullBright import inspect_read
            return {"pid": args.pid, "package": package.value, "moduleBase": hex(base),
                    "fullBright": inspect_read(read, base)}

        if args.always_day:
            from InspectAlwaysDay import inspect_read
            return {"pid": args.pid, "package": package.value, "moduleBase": hex(base),
                    "alwaysDay": inspect_read(read, base)}

        def site(rva, original):
            actual = read(base+rva, len(original))
            value = {"rva": hex(rva), "bytes": actual.hex(" ")}
            if actual == original:
                value["state"] = "original"
            elif actual[0] == 0xe9 and actual[5:] == bytes([0x90])*(len(original)-5):
                value["state"] = "jump"
                value["destination"] = hex(base+rva+5+struct.unpack_from("<i", actual, 1)[0])
            else:
                value["state"] = "unexpected"
            return value

        own = site(0x46B3201, bytes.fromhex("4c 39 e3 74 da 48 8b 03"))
        depth = site(0x1D55D5F, bytes.fromhex("c6 44 24 50 00"))
        hud = site(0x46B3178, bytes.fromhex("4d 85 e4 0f 94 c1 08 c1"))
        loop_exit = site(0x46B31F7, bytes.fromhex("0f 84 42 09 00 00"))
        names_mask = site(0x46B0120, bytes.fromhex("34 01 8b 8d b4 39 00 00"))
        result = {"pid": args.pid, "package": package.value, "moduleBase": hex(base),
                  "timestamp": hex(timestamp), "imageSize": hex(image_size),
                  "sites": {"ownPlayer": own, "depthMaterialArgument": depth,
                            "hudSuppression": hud, "actorLoopExit": loop_exit, "namesHudMask": names_mask}}
        if own["state"] == depth["state"] == "jump":
            bridge = int(own["destination"], 16)
            if int(depth["destination"], 16) == bridge+512:
                active, _, own_cb, depth_cb, continuation, skip, depth_continuation = struct.unpack(
                    "<II5Q", read(bridge+4096, 48))
                result["bridge"] = {
                    "address": hex(bridge), "activeCallbacks": active,
                    "ownCallback": hex(own_cb), "depthCallback": hex(depth_cb),
                    "destinationsVerified": (continuation == base+0x46B3209 and
                                             skip == base+0x46B31E0 and
                                             depth_continuation == base+0x1D55D64)}
                if hud["state"] == loop_exit["state"] == "jump":
                    hud_cb, loop_cb, hud_continue, loop_continue, other_labels, cleanup = struct.unpack(
                        "<6Q", read(bridge+4096+48, 48))
                    result["bridge"]["hudCallback"] = hex(hud_cb)
                    result["bridge"]["loopExitCallback"] = hex(loop_cb)
                    result["bridge"]["hudDestinationsVerified"] = (
                        int(hud["destination"], 16) == bridge+1024 and
                        int(loop_exit["destination"], 16) == bridge+1536 and
                        hud_continue == base+0x46B3180 and loop_continue == base+0x46B31FD and
                        other_labels == base+0x46B3B3F and cleanup == base+0x46B3FD7)
                    if names_mask["state"] == "jump":
                        mask_cb, mask_continue = struct.unpack("<2Q", read(bridge+4096+96, 16))
                        result["bridge"]["namesMaskCallback"] = hex(mask_cb)
                        result["bridge"]["namesMaskDestinationVerified"] = (
                            int(names_mask["destination"], 16) == bridge+2048 and mask_continue == base+0x46B0128)
        if args.options:
            options = args.options
            option = struct.unpack("<Q", read(options+0x28, 8))[0]
            vtable, owner = struct.unpack("<2Q", read(option, 16))
            pointer, _, key_length, capacity = struct.unpack("<4Q", read(owner+0x188, 32))
            values = struct.unpack("<4i", read(option+16, 16))
            name = read(pointer, 17) if key_length == 16 and 16 <= capacity <= 4096 else b""
            verified = (struct.unpack("<Q", read(options, 8))[0] == base+0xE7FC490 and
                        vtable == base+0xE778FE0 and name == b"game_thirdperson"+bytes([0]) and
                        values[0:2] == (2, 0) and 0 <= values[2] <= 2 and 0 <= values[3] <= 2)
            result["camera"] = {"options": hex(options), "verified": verified,
                                "perspective": values[2] if verified else None}
        return result
    finally:
        k.CloseHandle(handle)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pid", type=int)
    parser.add_argument("--rva", type=lambda value: int(value, 0))
    parser.add_argument("--size", type=lambda value: int(value, 0), default=4096)
    parser.add_argument("--coff")
    parser.add_argument("--options", type=lambda value: int(value, 0))
    parser.add_argument("--snapshot", help="Directory for local image-section snapshots")
    parser.add_argument("--always-day", action="store_true", help="Inspect the Always day profile and bridge")
    parser.add_argument("--full-bright", action="store_true", help="Inspect the Full Bright profile and bridge")
    print(json.dumps(inspect(parser.parse_args()), indent=2))
