# Supported Build Diagnostics

These tools were developed for the single profiled Minecraft Bedrock package `1.26.5203.0` x64. Before using them with another version, create and independently verify a new profile; the addresses listed in [FINDINGS.md](../FINDINGS.md) are not universal.

The Python scripts use the standard library. When reading a live process, they verify the supported package identity and open it only for querying and reading memory. They do not inject DLLs or modify game memory. `FovMemoryProbe.ps1` is also read-only, but it can scan a large memory range and saves the resulting snapshot to a local JSON file. PID is a required parameter and must not be copied from an old example.

`InspectNametag.py --snapshot` and the `--coff` option write local files/snapshots for analysis; these outputs are not part of the repository. Treat them as local diagnostic data and inspect their contents before sharing.

Examples for the currently supported build:

```powershell
$processId = Get-Process -Name Minecraft.Windows | Select-Object -First 1 -ExpandProperty Id
python diagnostics\InspectFov.py $processId
python diagnostics\InspectNametag.py $processId
python diagnostics\InspectNametag.py $processId --always-day
python diagnostics\InspectNametag.py $processId --full-bright
python diagnostics\InspectNametag.py $processId --snapshot artifacts\nametag-image
python diagnostics\InspectAlwaysDay.py --snapshot artifacts\nametag-image
python diagnostics\InspectFullBright.py --snapshot artifacts\nametag-image
python diagnostics\InspectScreenInput.py --snapshot artifacts\nametag-image
.\diagnostics\FovMemoryProbe.ps1 -Mode Capture -ProcessId $processId
```

A static inspection, snapshot, or diagnostic counter does not replace in-game behavior verification. The technical rationale for the current hooks and reference addresses for the supported build are in [FINDINGS.md](../FINDINGS.md); exact bytes are in `GameMod\*Profile.h`.

`InspectScreenInput.py` checks the native screen getters, transition producer, constructors, background name, and vtable bindings against an image snapshot with the supported PE identity. ScreenView `+0x460` is a transition flag; stable debug/toast overlays have zero there. The native cubemap is accepted only at the bottom of the shared stack, after verifying its separate vtable and fixed-name getter. The module's Zoom log records `screen_profile` and `screen_client`; zero means the profile or client is not available and shortcuts must remain disabled.

Default `C` Zoom in gameplay, typing `C` in chat without Zoom, returning to gameplay, `F6`–`F9` toggles, and `F10` safe detach were confirmed in a local single-player world. Logs also recorded Zoom restoration, rendering toggles, and hook restoration; the module was absent from the process after detach.

For the remaining live input checks, try a configured Zoom letter such as `F`, sign/anvil text entry, and the inventory/pause menus. Open chat while holding Zoom and confirm FOV restoration and normal UI scrolling. Close chat while keeping a shortcut held, then release and press it again; only the fresh gameplay press should activate it. Repeat with the other feature shortcuts temporarily configured to letters, and confirm that gameplay Zoom still captures its key and wheel without hotbar scrolling or false mouse movement. These additional checks have isolated test coverage but have not yet been performed in-game for the screen profile.
