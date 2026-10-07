# Specification and Current Limitations

## Purpose and Supported Build

Local add-on for official Minecraft Bedrock on Windows. The C# launcher verifies the game and prepares to load the custom C++ module. The module modifies only local controls and rendering; it is not a replacement client. This repository maintains an auditable implementation of Zoom and local rendering add-ons that can be built from the included source without third-party binary clients or modifications to the server or saved world.

The only supported package is `Microsoft.MinecraftUWP_1.26.5203.0_x64__8wekyb3d8bbwe`. Other versions are unsupported, and the launcher must reject them before installing hooks.

## Feature Scope

| Feature | Control | Behavior and verification status |
| --- | --- | --- |
| Zoom | C (hold), mouse wheel | Local FOV with a transition, restored when the key is released, Esc is pressed, or focus is lost. FOV and sensitivity change only after the target settings have been uniquely verified. The current input path has not yet been verified in-game since the global mouse hook was removed. |
| Always day | F6 | Keeps the local sun overhead at noon, the moon below the horizon, and the sky in its daytime phase without sunrise/sunset transitions. Changes only profiled temporary rendering inputs; world time continues normally. Basic brightening of the sky and terrain was verified at night. Celestial position, sky colours, stars, clouds, and restoration when disabled have not yet been visually verified. |
| Nametag | F7 | Custom name in both third-person views; uses the original renderer and depth testing for both text and background. Verified in a local game. |
| Status indicator | F9 | Show or hide the local indicator. |
| Safe detach | F10 | Restore modified values and hooks, then detach the module. Restoration must not be forced if code has been modified by another party or a bridge is still running. |
| Full Bright | F8 | Changes temporary parameters of the standard local light texture, not gamma, player state, or saved lighting. The user confirmed it works; other rendering modes and interaction with Always day have not been verified. |

Zoom values are configured in `[Zoom]` and feature shortcuts in `[Shortcuts]` in `config\bedrock-toolkit.ini`; the build script copies it next to the launcher. `FirstPersonFov` and `ThirdPersonFov` default to 15 and 28 respectively; the legacy `Fov` setting remains a fallback for both. All shortcuts must be supported and distinct. If `bedrock-toolkit.ini` is missing, the launcher and module use their defaults. If sensitivity, FOV, or camera perspective cannot be safely identified, the module must not attempt a blind write.

Always day and Full Bright start enabled after their rendering profiles are verified. Nametag starts enabled once valid camera options are available. Their configured shortcuts toggle them off and on. Zoom preparation starts automatically, but Zoom remains hold-to-use as described above.

## Security and Implementation Requirements

- Before any write, verify the exact package, PE identity, and complete original instructions with their relevant context. Reject the target on any mismatch, ambiguity, or read error.
- Do not treat FOV, sensitivity, or other heap addresses from a single session as stable identities. Find the correct named object and verify its structure, range, and uniqueness.
- Limit hooks to verified local input or rendering paths. Do not modify the server protocol, world time, saved world, player attributes, gamma, or persistent game objects.
- Prepare and verify installation of multiple patches as a coordinated operation. During restoration, compare both the installed patch and the expected original bytes; never overwrite another party's change. Before freeing bridge memory, wait for active callbacks to finish and verify game-thread instruction pointers. If restoration is unsafe, leave the module loaded and retry it.
- Before using an external package, verify its original source, version, license, build steps, and transitive dependencies. Prefer pinned source code and local builds. If its use is necessary, explicitly obtain the user's approval first, explain why it is required, and clearly describe identified risks and impacts; do not add or use the package without approval. MinHook source with its own license is included in `GameMod\vendor\minhook`. Do not add unknown or precompiled DLLs, third-party injectors, automatic downloads or updates, telemetry, or network communication by the add-on.
- Use only in a local single-player world. The project cannot reliably detect the game mode; multiplayer is unsupported.

## Build and Verification

Requirements and the build command are documented in [README.md](README.md). Tests can verify emitted bridges, registers, patching, and restoration against synthetic memory; by themselves, they do not establish behavior or visual results in a live game.

Read-only diagnostic tools for this build are in [diagnostics/](diagnostics/README.md). Reference RVAs and persistent findings for troubleshooting and future porting are in [FINDINGS.md](FINDINGS.md); the exact bytes for the supported profile remain in the source headers.

Compatibility claims apply only to the listed build. Multiplayer, other versions, dimensions, and graphics modes are not designated as supported. After changing the input path or rendering profile, distinguish successful builds, automated tests, and practical in-game verification again.
